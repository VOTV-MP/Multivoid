// coop/session/player_handshake_nick.cpp -- OUR OWN NAME, and the boundary its
// text crosses. Split out of player_handshake.cpp 2026-07-28 (arc D2 pushed that
// file to 826 LOC, past the 800 soft cap).
//
// WHY THESE FUNCTIONS ARE ONE FILE. They answer a single question -- "what name
// do we go by, and what text is a name allowed to contain" -- and they are the
// module's only consumers of coop/text. The handshake's other half is about
// SLOTS, EPOCHS and PAYLOAD FRAMING; nothing here touches a slot. The two stores
// are the reason it has to be one file rather than three: g_requestedNick (what
// the human typed) and g_localNick (what the host decided we are called) only
// make sense as a pair, and every rule about which one is persisted lives in
// AdoptCanonicalNickname below.
//
// Nothing moved namespace. These are declared in player_handshake_detail.h and
// player_handshake.h exactly as before, so no call site changed.

#include "coop/session/player_handshake.h"

#include "player_handshake_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/player/nickname_arbiter.h"
#include "coop/player/roster_ledger.h"
#include "coop/session/session_manager.h"
#include "coop/text/repertoire.h"
#include "coop/text/utf8_codec.h"
#include "ue_wrap/core/log.h"

#include <string>
#include <vector>

namespace coop::player_handshake {

namespace {

// T7 (ini rework): MY-NAME default from the shared registry constant. NOTE:
// SanitizeNickname's empty-result fallback below deliberately STAYS "Player" --
// that function runs on inbound REMOTE nicks too (symmetric defense), and a
// garbage remote nick must not render as our my-name default.
std::wstring g_localNick = coop::config_registry::MyNameDefaultW();
// What the human TYPED. The Join asks for this; g_localNick above is what we
// DISPLAY, which the host may rename for uniqueness (arc B). Two stores, one
// author each: the human owns the request, the host owns the display.
std::wstring g_requestedNick = g_localNick;
// Whether SanitizeNickname CHANGED the last request we set. See SetLocalNickname:
// the raw string is never stored, so this bit is the only surviving evidence that
// our own rules edited what the human asked for. AdoptCanonicalNickname needs it
// to decide whether a host-assigned suffix is safe to persist.
bool g_requestWasAltered = false;

}  // namespace

// ARC D / RULE 2: the encoder existed THREE times (here, chat_sync::NickUtf8,
// chat_feed::ToUtf8). These are thin adapters over the ONE owner in coop/text --
// kept only because the wire code speaks vector<uint8_t>.
std::vector<uint8_t> ToUtf8(const std::wstring& w) {
    const std::string s = coop::text::ToUtf8(w);
    return std::vector<uint8_t>(s.begin(), s.end());
}

// STRICT on the way in. These bytes came from a PEER, so an ill-formed field is
// refused WHOLE rather than repaired: a repair invents a name nobody chose, and
// MultiByteToWideChar without MB_ERR_INVALID_CHARS silently substitutes U+FFFD.
// Until arc D the ASCII allowlist was the only thing destroying ill-formed bytes,
// so removing it without this would have made bad input merely ugly. An empty
// return reads as "no name" and the caller's placeholder takes over.
std::wstring FromUtf8(const uint8_t* p, int len) {
    if (len <= 0) return {};
    std::wstring out;
    if (!coop::text::FromUtf8Strict(reinterpret_cast<const char*>(p),
                                    static_cast<size_t>(len), &out)) {
        UE_LOGW("handshake: refused an ill-formed UTF-8 text field (%d bytes)", len);
        return {};
    }
    return out;
}

// Nickname sanitizer (2026-05-25, VT-inspired): trust-boundary defense at
// the nameplate / hud-feed display surface. Borrowed from VoidTogether-
// Server's utilityModule.js SimplifyName (regex /^-+|-+$|[^A-Za-z0-9 ]+/g
// + truncate to 20) -- adjusted for our wchar_t pipeline and to ALLOW
// internal spaces (single-space runs collapsed; leading/trailing trimmed).
//
// Why: a peer's Join reliable payload carries an arbitrary UTF-8 byte
// string of arbitrary length. Without sanitization, a malicious or buggy
// peer could inject:
//   - Control chars (newline / null / ANSI escape) that corrupt our
//     chat-feed / nameplate text rendered by our ImGui HUD.
//   - Right-to-left override unicode (U+202E) that visually inverts the
//     subsequent text in the nameplate.
//   - Combining diacritics that render glyphs taller than the widget bg.
//   - Very long strings that overflow the floating nameplate beyond the
//     screen and waste reliable-channel bandwidth on join.
//
// Pattern: keep ASCII alphanumerics + space + the safe punctuation set
// `[-_.]`. Strip everything else. Collapse multi-space runs to single.
// Trim leading/trailing whitespace + dashes. Truncate to 20 wchars max.
// Empty result falls back to "Player" so the nameplate isn't blank.
//
// This applies SYMMETRICALLY to both the inbound Join receive (defense
// against peer-side garbage) AND to our own outbound SetLocalNickname
// (defense against env-var typos / Windows path leakage / our own
// future bugs).
//
// NOT a profanity filter -- VoidTogether uses obscenity.js for that
// (450 KB of regexes + transformers). Out of scope for the standalone
// C++ mod; a strict-character sanitizer + length cap is the trust-
// boundary fix, profanity moderation is a separate moderation feature
// pending Phase 6+ (per the VT adoption findings ranked shortlist).
std::wstring SanitizeNickname(const std::wstring& raw) {
    // ARC D: a DENYLIST. See the note above -- an allowlist cannot survive a
    // widening alphabet, because the script we forgot fails silently.
    //
    // ARC D2 replaced four hand-written rows (U+200B-200F, U+202A-202E,
    // U+2066-2069, U+FEFF) with the Unicode PROPERTY they were each sampling:
    // Default_Ignorable_Code_Point. The rows were not wrong, they were
    // incomplete in a way nobody could see from reading them -- U+034F (CGJ) has
    // advance 0 in Fixedsys AND Roboto, the two default families, and sailed
    // through, so `Ann͏a` carried a distinct fold key and identical pixels. An
    // enumeration of the invisibles we happened to think of is a site list; the
    // property is the invariant (RULE 1).
    auto denied = [](uint32_t c) {
        if (c < 0x20 || c == 0x7F) return true;          // C0 controls + DEL
        if (c >= 0xD800 && c <= 0xDFFF) return true;     // unpaired surrogate
        return coop::text::IsDefaultIgnorable(c);
    };
    // RULE 2, commit 2 (2026-07-30): the literal `c >= 0x0300 && c <= 0x036F`
    // that used to sit here is GONE. It was correct while the Latin block was the
    // only mark range that could bake -- and it became silently wrong the moment
    // the marks were admitted, because Thaana, Tamil, Thai, Arabic and Hebrew
    // marks all draw now and none of them is in 0x0300-0x036F. A hand-written
    // range beside a generated table is two owners of one fact, and the hand
    // written one loses without a symptom: the rule below would have policed
    // Latin diacritics and let the other five scripts stack onto the UI.
    std::wstring out;
    out.reserve(raw.size());
    bool lastWasSpace = true;  // primes the leading-space trim
    // In CODEPOINTS. Iterating wchar_t units cannot see a supplementary-plane
    // character at all, so every tag character in U+E0000-E0FFF -- all of them
    // ignorable -- would have passed the denylist as two anonymous halves.
    for (size_t i = 0; i < raw.size(); ) {
        uint32_t c = 0;
        const size_t units = coop::text::DecodeCodepoint(raw, i, &c);
        const wchar_t* at = raw.data() + i;
        i += units;
        if (denied(c)) continue;
        if (c == L' ') {
            if (!lastWasSpace) { out.push_back(L' '); lastWasSpace = true; }
            continue;
        }
        // A combining mark with nothing to combine with stacks onto whatever the
        // UI drew before the name. Only position 0 -- a mark in the MIDDLE of a
        // name is legitimate text in five scripts and passes untouched.
        if (out.empty() && coop::text::IsCombiningMark(c)) continue;
        out.append(at, units);
        lastWasSpace = false;
    }
    // Cap in CODEPOINTS, never in wchar_t units: the old cap could cut an astral
    // character in half and leave an unpaired surrogate on the wire.
    out = coop::text::CapCodepoints(out, kNickMaxChars);
    // Trim trailing space.
    while (!out.empty() && (out.back() == L' ' || out.back() == L'-'))
        out.pop_back();
    // Trim leading dashes (the SimplifyName regex's ^-+ rule -- VT
    // didn't trim leading space because their regex stripped all space
    // implicitly; we kept internal spaces so leading-space is already
    // gone via the `lastWasSpace=true` prime).
    size_t start = 0;
    while (start < out.size() && out[start] == L'-') ++start;
    if (start > 0) out.erase(0, start);
    return out.empty() ? std::wstring(L"Player") : out;
}

void SetLocalNickname(const std::wstring& nick) {
    // VT-inspired sanitize-on-input (2026-05-25): symmetric defense.
    // Sanitizing here too means our env-var setup (VOTVCOOP_NET_NICK)
    // can't accidentally send garbage over the wire that we then
    // sanitize on the OTHER end -- net is cleaner if both ends agree
    // on the displayable form.
    if (nick.empty()) return;
    // Both stores: this is a fresh REQUEST, and until a host arbitrates it the
    // requested name IS the displayed one. Splitting them here is what lets a
    // host rename us without the next session re-asking for the suffix.
    g_requestedNick = SanitizeNickname(nick);
    g_localNick = g_requestedNick;
    // DID OUR OWN RULES ALTER THE REQUEST? Recorded HERE because this is the only
    // moment the raw string exists -- the store above is already sanitized, so
    // every later reader is structurally blind to whatever was removed. That
    // blindness is not hypothetical: the repertoire scan in
    // AdoptCanonicalNickname reads this same sanitized store, so a name that lost
    // a leading mark scans perfectly clean.
    //
    // A second store holding the raw string would need its own sanitization at
    // every other use, so what is kept is the one BIT that the decision needs.
    g_requestWasAltered = (g_requestedNick != nick);
}

void AdoptCanonicalNickname(const std::wstring& canonical) {
    // ARC B: the host assigned this. g_localNick is the single store that
    // chat_sync.cpp:128, peer_action_feed.cpp:51, both of roster.cpp's local-row
    // reads (:73, :121) and the nameplate all derive from, so writing it is the
    // whole DISPLAY half of the handback.
    if (canonical.empty() || canonical == g_localNick) return;
    const std::wstring asked = g_requestedNick;
    g_localNick = canonical;

    // ...and the KEEPING half (user decision 2026-07-28). The name is now ours:
    // it becomes what we ask for, in this process and in the ini, so the next
    // session opens as Pelmentor2 and stays Pelmentor2 unless someone else is
    // already using it. Writing all three stores in one place is deliberate --
    // a name that is displayed but not requested would silently revert on the
    // next launch, which is exactly the "temporary" behaviour that was rejected.
    if (canonical == g_requestedNick) return;

    // ARC D2 -- THE PERSIST SPLIT, and it is decided HERE, locally, with no new
    // wire kind and no kProtocolVersion bump.
    //
    // The arbiter now collides names that merely LOOK alike: every codepoint
    // this build cannot draw folds to one sentinel, so two CJK names take a
    // suffix even though they share no character. That suffix is a fact about
    // OUR FONT SET, not about the human -- and a later build that embeds more
    // scripts would stop producing it. Persisting it would make a rendering
    // artifact permanent: the user would be Zhang2 forever, in an install that
    // can draw 张伟 perfectly well.
    //
    // Only the receiving peer can tell the two apart, because only it knows what
    // it ASKED for. A genuine string clash (two literal "Pelmentor") is the
    // user's own name meeting someone else's and is kept, per the decision
    // above. A repertoire-suspect request -- one containing any codepoint the
    // fold sentinels -- keeps DISPLAYING the assigned name and keeps REQUESTING
    // the original. See [[lesson-a-placeholder-must-never-become-an-identity]]:
    // the last time a derived string was allowed to become the stored identity,
    // a joiner learned "Player" and wrote it over its real name forever.
    // The predicate must match what FoldKey ACTUALLY does, not what the
    // repertoire says. U+FFFD is IN the repertoire (it is the fallback glyph and
    // must be baked), so a name containing a literal one is "drawable" here while
    // its fold key is pure sentinel -- it would collide with any CJK name, take a
    // suffix, and PERSIST it. Same test, both sides.
    bool repertoireSuspect = false;
    for (size_t i = 0; i < asked.size() && !repertoireSuspect; ) {
        uint32_t cp = 0;
        i += coop::text::DecodeCodepoint(asked, i, &cp);
        if (!coop::text::InRepertoire(cp) || cp == coop::nickname_arbiter::kAbsentSentinel)
            repertoireSuspect = true;
    }
    // ...AND THE OTHER WAY OUR OWN RULES CAN EARN A SUFFIX (commit 2). The scan
    // above asks "does the request contain something we cannot DRAW". It cannot
    // ask "did we EDIT the request", because it reads the already-sanitized store
    // -- so a name that lost a leading combining mark, or an ignorable, or a
    // control character scans perfectly clean, takes the branch below, and writes
    // a suffix into multivoid.ini that the human never asked for and cannot see
    // the cause of. Same class as the repertoire case: a local artefact, not a
    // fact about the human, so it displays but does not persist.
    // [[lesson-a-placeholder-must-never-become-an-identity]]
    if (repertoireSuspect || g_requestWasAltered) {
        UE_LOGI("nick: host renamed us '%ls' -> '%ls' (display only -- %s, so the "
                "suffix is a local artefact and is NOT persisted)",
                asked.c_str(), canonical.c_str(),
                repertoireSuspect ? "the request contains characters this build "
                                    "cannot draw"
                                  : "our own sanitizer altered the request");
        return;
    }

    g_requestedNick = canonical;
    const std::vector<uint8_t> u8 = ToUtf8(canonical);
    const std::string nickUtf8(reinterpret_cast<const char*>(u8.data()), u8.size());
    coop::session_manager::SetNickname(nickUtf8);  // the browser field shows it too
    coop::config::WriteIniValue(coop::config_registry::rows::net_nick, nickUtf8.c_str());
    UE_LOGI("nick: host renamed us '%ls' -> '%ls' (kept: written to multivoid.ini)",
            asked.c_str(), canonical.c_str());
}

const std::wstring& LocalNickname() { return g_localNick; }

const std::wstring& RequestedNickname() { return g_requestedNick; }

const std::wstring& NicknameForSlot(int slot) {
    // Thin ledger read (arc A T9): the signature is unchanged on purpose so the
    // ~14 call sites across 9 files keep compiling untouched. The placeholder
    // fallback lives in the ledger now -- it is the ONE copy.
    return coop::roster_ledger::DisplayName(slot);
}

}  // namespace coop::player_handshake
