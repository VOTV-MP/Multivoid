// coop/session/player_handshake_nick.cpp -- our own name, and the boundary its text crosses:
// what name we go by, and what text a name may contain. The two stores only make sense as a
// pair, the requested name (what the person typed) and the displayed name (what the host
// decided we are called), and every rule about which one is persisted lives in
// AdoptCanonicalNickname. Declared in player_handshake.h and player_handshake_detail.h.

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

// The my-name default from the shared registry constant. SanitizeNickname's empty-result
// fallback stays "Player" on purpose: it runs on inbound remote nicks too, and a garbage
// remote nick must not render as our own default.
std::wstring g_localNick = coop::config_registry::MyNameDefaultW();
// What the person typed. The Join asks for this; g_localNick is what we display, which the
// host may rename for uniqueness. Two stores, one author each: the person owns the request,
// the host owns the display.
std::wstring g_requestedNick = g_localNick;
// Whether SanitizeNickname changed the last request we set: the raw string is never stored, so
// this bit is the only evidence that our own rules edited what the person asked for.
// AdoptCanonicalNickname needs it to decide whether a host-assigned suffix may persist.
bool g_requestWasAltered = false;

}  // namespace

// A thin adapter over the one encoder in coop/text, kept because the wire code speaks
// vector<uint8_t>.
std::vector<uint8_t> ToUtf8(const std::wstring& w) {
    const std::string s = coop::text::ToUtf8(w);
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Strict on the way in: the bytes came from a peer, so an ill-formed field is refused whole
// rather than repaired, since a repair invents a name nobody chose (a lenient decode would
// silently substitute U+FFFD). An empty return reads as no name, and the caller's placeholder
// takes over.
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

// The nickname sanitizer, the trust-boundary defence at the nameplate and feed surfaces; its
// shape follows VoidTogether-Server's SimplifyName (trim, cap), adapted to codepoints and to
// allow internal spaces. A peer's Join carries an arbitrary byte string of arbitrary length,
// which could otherwise inject control characters into the feed, a right-to-left override
// into the nameplate, combining marks that stack above the widget, or a string that overflows
// the plate and the join. Applied symmetrically to the inbound Join and to our own outbound
// name. Not a profanity filter.
std::wstring SanitizeNickname(const std::wstring& raw) {
    // A denylist: an allowlist cannot survive a widening alphabet, because the script it forgot
    // fails silently. The invisibles are the Unicode Default_Ignorable_Code_Point property rather
    // than a hand-written list of ranges: an enumeration of the invisibles one happens to think of
    // is a site list, and one it missed (the combining grapheme joiner, advance 0 in both default
    // families) gave two identical-looking names distinct fold keys.
    auto denied = [](uint32_t c) {
        if (c < 0x20 || c == 0x7F) return true;          // C0 controls + DEL
        if (c >= 0xD800 && c <= 0xDFFF) return true;     // unpaired surrogate
        return coop::text::IsDefaultIgnorable(c);
    };
    // No hand-written mark range here: five scripts' marks draw now, and a range beside the
    // generated table would be a second owner of one fact, silently policing Latin diacritics
    // alone.
    std::wstring out;
    out.reserve(raw.size());
    bool lastWasSpace = true;  // primes the leading-space trim
    // In codepoints: iterating wchar_t units cannot see a supplementary-plane character, so every
    // tag character, all ignorable, would pass the denylist as two anonymous halves.
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
        // A combining mark with nothing to combine with stacks onto whatever the UI drew before the
        // name. Only at position 0: a mark in the middle is legitimate text in five scripts.
        if (out.empty() && coop::text::IsCombiningMark(c)) continue;
        out.append(at, units);
        lastWasSpace = false;
    }
    // The cap is in codepoints, never wchar_t units, or an astral character could be cut in half
    // and leave an unpaired surrogate on the wire.
    out = coop::text::CapCodepoints(out, kNickMaxChars);
    // Trailing spaces and dashes.
    while (!out.empty() && (out.back() == L' ' || out.back() == L'-'))
        out.pop_back();
    // Leading dashes; leading spaces are already gone through the primed space flag.
    size_t start = 0;
    while (start < out.size() && out[start] == L'-') ++start;
    if (start > 0) out.erase(0, start);
    return out.empty() ? std::wstring(L"Player") : out;
}

void SetLocalNickname(const std::wstring& nick) {
    // Sanitised on input too, so our own env setup cannot send garbage the other end then
    // sanitises; both ends agree on the displayable form.
    if (nick.empty()) return;
    // Both stores: this is a fresh request, and until a host arbitrates it the requested name is
    // the displayed one. Splitting them here is what lets a host rename us without the next
    // session re-asking for the suffix.
    g_requestedNick = SanitizeNickname(nick);
    g_localNick = g_requestedNick;
    // Did our own rules alter the request? Recorded here, the only moment the raw string exists:
    // the store is already sanitised, so every later reader is blind to whatever was removed, and
    // the repertoire scan in AdoptCanonicalNickname reads this same store. One bit is kept rather
    // than a second raw store that would need its own sanitisation at every use.
    g_requestWasAltered = (g_requestedNick != nick);
}

void AdoptCanonicalNickname(const std::wstring& canonical) {
    // The host assigned this. g_localNick is the single store the chat, the action feed, the
    // roster's local row and the nameplate derive from, so writing it is the whole display half.
    if (canonical.empty() || canonical == g_localNick) return;
    const std::wstring asked = g_requestedNick;
    g_localNick = canonical;

    // The keeping half: the name is now ours, so it becomes what we ask for, in this process and in
    // the ini, and the next session opens under it unless someone else already uses it. A name
    // displayed but not requested would silently revert on the next launch.
    if (canonical == g_requestedNick) return;

    // The persist split, decided here with no new wire kind. The arbiter collides names that
    // merely look alike: every codepoint this build cannot draw folds to one sentinel, so two CJK
    // names take a suffix even though they share no character. That suffix is a fact about our
    // font set, not about the person, and a later build that embeds more scripts would stop
    // producing it; persisting it would make a rendering artefact permanent. Only the receiving
    // peer can tell the two apart, because only it knows what it asked for: a genuine string
    // clash is kept, while a repertoire-suspect request (any codepoint the fold sentinels) keeps
    // displaying the assigned name and keeps requesting the original. The predicate must match
    // what the fold does, not what the repertoire says: U+FFFD is in the repertoire (it is the
    // fallback glyph) while its fold key is pure sentinel.
    bool repertoireSuspect = false;
    for (size_t i = 0; i < asked.size() && !repertoireSuspect; ) {
        uint32_t cp = 0;
        i += coop::text::DecodeCodepoint(asked, i, &cp);
        if (!coop::text::InRepertoire(cp) || cp == coop::nickname_arbiter::kAbsentSentinel)
            repertoireSuspect = true;
    }
    // The other way our own rules can earn a suffix: the scan above asks whether the request holds
    // something we cannot draw, and cannot ask whether we edited it, since it reads the sanitised
    // store; a name that lost a leading mark, an ignorable or a control character scans clean. The
    // same class as the repertoire case: a local artefact, so it displays but does not persist.
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
    // A thin ledger read; the placeholder fallback lives in the ledger, the one copy.
    return coop::roster_ledger::DisplayName(slot);
}

}  // namespace coop::player_handshake
