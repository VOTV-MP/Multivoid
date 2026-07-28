// coop/player_handshake.cpp -- see coop/player_handshake.h.

#include "coop/session/player_handshake.h"

#include "player_handshake_detail.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"           // arc B: persist a host-assigned name
#include "coop/config/config_registry.h"  // T7: the my-name default constant
#include "coop/text/utf8_codec.h"
#include "coop/session/session_manager.h"
#include "coop/moderation/seen_players.h"

#include "coop/element/player.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/local_body.h"
#include "coop/player/nameplate.h"
#include "coop/player/hand_item.h"  // v105: hand-item mirrors reset/slot-disconnect
#include "coop/player/nick_color.h"
#include "coop/player/nickname_arbiter.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/player/skin_registry.h"
#include "coop/version.h"                // v122: kGameTarget (the Join game field)
#include "ue_wrap/core/hot_path_guard.h"
#include "coop/comms/chat_bubbles.h"
#include "coop/comms/chat_feed.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
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

// v73 per-player inventory identity: OUR guid, which rides our outbound Join.
// ASCII 32-hex; not displayed.
std::string g_localGuid;

// ARC A / RULE 2: the FIVE per-slot side-tables that used to live here --
// g_remoteNickBySlot, g_joinSentBySlot, g_joinAnnouncedBySlot, g_guidBySlot,
// g_skinBySlot -- are GONE. They were five parallel arrays keyed by peer slot,
// each needing its own line in OnSlotDisconnected to avoid attributing a
// departed person's state to the next occupant of their slot, and the answer to
// "did we get them all?" was a checklist.
//
// Four are LEDGER ROW FIELDS now (nick / guid / skin / joinAnnounced): they
// describe the PERSON, so they die with the row.
//
// joinSent is the one that is NOT, and the distinction is worth the paragraph:
// it records whether OUR Join has gone out over a LINK, and a client sends its
// Join to slot 0 before it has any idea who is on the other end -- row 0 is born
// later, from the host's own Join. A row field would silently refuse the write
// (setters ignore unoccupied slots) and the client would re-send its Join every
// tick forever. So it is per-slot state that must still be cleared when the
// occupant changes: exactly what PerSlotState exists for. Declaring it through
// that type IS the whole wiring -- it registers its own clear in its
// constructor, so it cannot be forgotten the way the five arrays were.
coop::roster_ledger::PerSlotState<bool> g_joinSent;

}  // namespace

// Store + live-apply: if the described slot's puppet is already spawned, re-skin
// it NOW (mid-session SkinChange, or a Join that raced the first pose).
// External linkage (player_handshake_detail.h): HandleSkinChange lives in
// player_handshake_prefs.cpp; the side-table stays owned HERE.
void StoreSkinForSlot(int slot, std::string name) {
    if (slot < 0 || slot >= net::kMaxPeers) return;
    if (coop::roster_ledger::Get(slot).skin == name) return;
    coop::roster_ledger::SetSkin(slot, std::move(name));
    if (RemotePlayer* p = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot)))
        p->ApplySkin(coop::roster_ledger::Get(slot).skin);
}

// Parse one [u8 len][ASCII] field. Returns bytes consumed (0 = malformed/absent);
// `out` untouched unless a well-formed non-empty field validated as a skin name.
// External linkage (player_handshake_detail.h): shared with HandleSkinChange.
size_t ParseSkinField(const uint8_t* p, size_t remaining, std::string* out) {
    if (remaining < 1) return 0;
    const int len = p[0];
    if (1 + len > static_cast<int>(remaining)) return 0;
    if (len > 0) {
        std::string name(reinterpret_cast<const char*>(p + 1), static_cast<size_t>(len));
        // Boundary rule: the name becomes a LoadObject package path component.
        if (coop::skins::IsValidSkinName(name))
            *out = std::move(name);
        else
            UE_LOGW("handshake: peer sent an invalid skin name (%d bytes) -- ignored", len);
    }
    return 1 + static_cast<size_t>(len);
}

namespace {

// v94 display-prefs flags byte (appended to Join + PlayerJoined after the skin
// field). Bit layout is the wire contract -- extend with new bits, never re-order.
constexpr uint8_t kPrefNameplateVisible = 0x01;

uint8_t BuildLocalPrefsFlags() {
    uint8_t f = 0;
    if (coop::nameplate::LocalVisible()) f |= kPrefNameplateVisible;
    return f;
}

}  // namespace [the LOCAL prefs byte only; everything below is EXTERNAL and
   //            shared with the sibling TUs via player_handshake_detail.h]

uint8_t PrefsFlagsForSlot(int slot) {
    uint8_t f = 0;
    if (coop::nameplate::VisibleForSlot(slot)) f |= kPrefNameplateVisible;
    return f;
}

void StorePrefsFlagsForSlot(int slot, uint8_t flags) {
    coop::nameplate::StoreVisibleForSlot(slot, (flags & kPrefNameplateVisible) != 0);
}

// v103 nick color (12f): a self-describing [u8 has][u8 r][u8 g][u8 b] field
// appended to Join + RosterRow after the prefs flags byte. has=0 -> the 3
// color bytes are present but ignored (fixed 4-byte field, field-by-field
// parse discipline).
void AppendNickColorField(std::vector<uint8_t>& out, uint32_t packed) {
    out.push_back(coop::nick_color::IsCustom(packed) ? 1 : 0);
    out.push_back(coop::nick_color::R(packed));
    out.push_back(coop::nick_color::G(packed));
    out.push_back(coop::nick_color::B(packed));
}

// Parse the 4-byte color field at `p` and store for `slot`. Returns bytes
// consumed (0 = absent/truncated -> slot color untouched).
size_t ParseNickColorField(const uint8_t* p, size_t remaining, int slot) {
    if (remaining < 4) return 0;
    coop::nick_color::StoreForSlot(
        slot, p[0] != 0 ? coop::nick_color::Pack(p[1], p[2], p[3]) : 0u);
    return 4;
}

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
    auto denied = [](wchar_t c) {
        if (c < 0x20 || c == 0x7F) return true;       // C0 controls + DEL
        if (c >= 0x200B && c <= 0x200F) return true;  // zero-width + LRM/RLM
        if (c >= 0x202A && c <= 0x202E) return true;  // bidi embeddings/overrides
        if (c >= 0x2066 && c <= 0x2069) return true;  // bidi isolates
        if (c == 0xFEFF) return true;                 // BOM / zero-width no-break
        return false;
    };
    auto combining = [](wchar_t c) { return c >= 0x0300 && c <= 0x036F; };

    std::wstring out;
    out.reserve(raw.size());
    bool lastWasSpace = true;  // primes the leading-space trim
    for (wchar_t c : raw) {
        if (denied(c)) continue;
        if (c == L' ') {
            if (!lastWasSpace) { out.push_back(L' '); lastWasSpace = true; }
            continue;
        }
        // A combining mark with nothing to combine with stacks onto whatever the
        // UI drew before the name.
        if (out.empty() && combining(c)) continue;
        out.push_back(c);
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

void SetLocalGuid(const std::string& guid) { g_localGuid = guid; }

const std::string& GuidForSlot(int slot) {
    return coop::roster_ledger::Get(slot).guid;  // GT-asserted inside the ledger
}

const std::string& SkinForSlot(int slot) {
    return coop::roster_ledger::Get(slot).skin;  // GT-asserted inside the ledger
}

void Reset() {
    // The per-slot identity state is the ledger's; clearing a row fires the
    // transition, which is what drives every registered teardown (the four
    // display-pref side-tables below among them). Belt-and-braces at session
    // START -- the load-bearing clear is ClearAll at session STOP.
    coop::roster_ledger::Reset();
    coop::hand_item::Reset();  // v105: destroy hand-item display mirrors + states
}

void MaybeSendJoinToSlot(net::Session& session, int slot,
                         std::vector<uint8_t>& joinPayload,
                         bool& joinPayloadBuilt) {
    // Reads g_localNick + the ledger row's joinSent latch. Called from the
    // net_pump connect-edge each tick. (The boot-thread WRITER SetLocalNickname
    // stays unguarded -- it runs once on the bringup thread before the pump
    // sends any Join.)
    UE_ASSERT_GAME_THREAD("g_localNick/g_joinSent (MaybeSendJoinToSlot)");
    if (slot < 0 || slot >= net::kMaxPeers) return;
    if (g_joinSent[slot]) return;
    // v13 (A4 2026-05-29): hold off on the FIRST Join until our
    // own Player Element is allocated. Otherwise the Join goes
    // out with senderElementId=0 and the receiver can't install
    // a mirror, leaving cross-peer Registry::Get(senderElementId)
    // unable to resolve for the lifetime of the session (only
    // disconnect+reconnect would re-fire the Join). The wait is
    // bounded: client side allocates after AssignPeerSlot lands
    // (~one extra net pump tick at 125 Hz, ~8 ms). Host side has
    // its Element allocated at net pump startup so this guard
    // only briefly skips at boot.
    const coop::element::ElementId selfEidProbe =
        coop::players::Registry::Get().LocalPlayerElementId();
    if (selfEidProbe == coop::element::kInvalidId) {
        return;  // retry next tick
    }
    if (!joinPayloadBuilt) {
        // v16 prefix: [uint32 senderElementId] then [uint8 nicklen][nick
        // UTF-8]. Receiver RegisterMirrors senderElementId into the
        // sender's peer slot so wire packets bearing senderElementId
        // resolve via Registry::Get on the receiver. (v14 added an
        // 8-bit senderContext byte after senderElementId; v16
        // PR-FOUNDATION-1b moved stale-gen defense to the packet header's
        // senderEpoch and removed the byte.)
        const uint32_t selfEidWire = selfEidProbe;
        joinPayload.resize(4);
        std::memcpy(joinPayload.data(), &selfEidWire, 4);
        // Ask for what the human typed, never for a suffix the host handed us last
// time -- otherwise a reconnect would ratchet Pelmentor2 -> Pelmentor3.
        // Cap on a CHARACTER boundary. A raw resize() here manufactures exactly
        // the ill-formed tail the receive boundary above refuses -- i.e. OUR OWN
        // name would reach the host as the placeholder.
        const std::string nickStr = coop::text::CapUtf8Bytes(
            coop::text::ToUtf8(g_requestedNick), coop::text::kNickMaxBytes);
        std::vector<uint8_t> nickUtf8(nickStr.begin(), nickStr.end());
        UE_LOGI("handshake: Join to slot %d asks for '%ls' (%zu bytes)",
                slot, g_requestedNick.c_str(), nickUtf8.size());
        joinPayload.push_back(static_cast<uint8_t>(nickUtf8.size()));
        joinPayload.insert(joinPayload.end(), nickUtf8.begin(), nickUtf8.end());
        // v73 per-player inventory: append [uint8 guidlen][guid ASCII] after the nick. The
        // HOST keys this peer's inventory file by it. ASCII hex (<=255); absent -> empty.
        const uint8_t guidLen = static_cast<uint8_t>(g_localGuid.size() > 255 ? 255 : g_localGuid.size());
        joinPayload.push_back(guidLen);
        joinPayload.insert(joinPayload.end(), g_localGuid.begin(), g_localGuid.begin() + guidLen);
        // v93 skins: append [uint8 skinlen][skin ASCII] after the guid -- the at-join
        // skin announce (local_body owns the local choice; validated <=48 chars).
        const std::string& skin = coop::local_body::LocalSkinName();
        const uint8_t skinLen = static_cast<uint8_t>(skin.size() > 48 ? 48 : skin.size());
        joinPayload.push_back(skinLen);
        joinPayload.insert(joinPayload.end(), skin.begin(), skin.begin() + skinLen);
        // v94 display prefs: [u8 flags] after the skin (bit0 = nameplate visible) --
        // the at-join announce that keeps LATE JOINERS in agreement with a peer that
        // hid its plate before they arrived (the user's "no ghost plate" ask).
        joinPayload.push_back(BuildLocalPrefsFlags());
        // v103 nick color (12f): [u8 has][u8 r][u8 g][u8 b] after the flags byte --
        // the at-join color announce (nick_color owns the local choice).
        AppendNickColorField(joinPayload, coop::nick_color::LocalPacked());
        // v122 version identity: [u8 gamelen][game ASCII] after the color field --
        // the sender's VOTV game target (the OTHER identity half, the build number,
        // already rides the packet HEADER as the protocol version). The receiver
        // byte-equality-validates it at the top of HandleJoinMessage (the wire-level
        // Minecraft-shape gate that also covers direct connect / env boot, where no
        // browser row pre-flight ran). Constant is compile-time <=23 chars.
        {
            const char* game = coop::version::kGameTarget;
            const size_t gameLen = std::min<size_t>(std::strlen(game), 23);
            joinPayload.push_back(static_cast<uint8_t>(gameLen));
            joinPayload.insert(joinPayload.end(), game, game + gameLen);
        }
        joinPayloadBuilt = true;
    }
    if (session.SendReliableToSlot(slot, net::ReliableKind::Join,
                                   joinPayload.data(),
                                   static_cast<int>(joinPayload.size()))) {
        g_joinSent[slot] = true;
    }
    // If send fails (transient), the caller will hit this slot again next tick.
}

namespace {

// The person-state teardown that used to be OnSlotDisconnected, now driven by
// the LEDGER ROW TRANSITION instead of by a disconnect callback. Two things
// changed and both matter:
//   - It fires on a REPLACEMENT too, not only on a departure. A recycled slot
//     goes X -> Y with no absence in between, so the old callback could be
//     skipped entirely and Y would inherit X's skin, colour, plate and bubble.
//   - It receives SNAPSHOTS, so it no longer depends on running BEFORE the
//     nickname is cleared. That ordering used to be load-bearing and documented
//     as such; the "<X> left the game" line had to print first.
void OnSlotReplaced_TearDownPerson(int slot, const coop::roster_ledger::Row& outgoing,
                                   const coop::roster_ledger::Row& /*incoming*/) {
    if (!outgoing.occupied()) return;  // nothing was there; nothing to tear down
    coop::nameplate::OnSlotDisconnected(slot);   // v94: plate pref back to visible
    coop::nick_color::OnSlotDisconnected(slot);  // v103: nick color back to default
    coop::chat_bubbles::OnSlotDisconnected(slot);  // 12g: no inherited bubble
    coop::hand_item::OnSlotDisconnected(static_cast<uint8_t>(slot));  // v105: drop the hand mirror
}

}  // namespace

void InstallLedgerSubscribers() {
    // Idempotent inside the ledger (dedupes by function pointer), so a
    // Stop()/Start() cycle in the same process cannot double-register and
    // double-fire every teardown.
    coop::roster_ledger::SubscribeSlotReplaced(&OnSlotReplaced_TearDownPerson);
    InstallRosterPulseSubscriber();  // player_handshake_roster.cpp
}

const std::wstring& NicknameForSlot(int slot) {
    // Thin ledger read (arc A T9): the signature is unchanged on purpose so the
    // ~14 call sites across 9 files keep compiling untouched. The placeholder
    // fallback lives in the ledger now -- it is the ONE copy.
    return coop::roster_ledger::DisplayName(slot);
}

bool IsValidGuid(const std::string& guid) {
    if (guid.size() != 32) return false;
    for (char c : guid) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

bool HandleJoinMessage(net::Session& session,
                       const net::Session::ReliableMessage& msg) {
    // Writes the sender's LEDGER ROW. Dispatched from event_feed::Update on the
    // game thread (net_pump::Tick -> Update), which reconciles the ledger BEFORE
    // draining reliables -- so on the host the sender's row already exists by the
    // time its Join lands here (the Join arrives over a configured lane, so the
    // slot is ready, which is exactly the reconcile's birth condition).
    UE_ASSERT_GAME_THREAD("ledger row (HandleJoinMessage)");
    const int senderSlot = msg.senderPeerSlot;
    if (senderSlot < 0 || senderSlot >= net::kMaxPeers) {
        UE_LOGW("player_handshake: Join has invalid senderPeerSlot=%d -- dropping",
                senderSlot);
        return true;
    }
    // v16 (PR-FOUNDATION-1b 2026-05-29): parse [uint32 senderElementId]
    // prefix then the existing [uint8 nicklen][nick UTF-8]. The protocol
    // version bump (v15->v16) at ParseHeader guarantees pre-v16 senders
    // can't misalign through here -- their packets are rejected upstream.
    //
    // W-3 (2026-05-29 audit): reject undersized Join instead of degrading to
    // no-mirror routing. Silent degradation leaves the peer permanently
    // without senderEpoch-based stale-gen defense; safer to drop a
    // malformed Join and let the sender's retry produce a well-formed one.
    if (msg.payloadLen < 4) {
        UE_LOGW("player_handshake: Join payload %zu B too short for v16 prefix "
                "(senderSlot=%d) -- dropping",
                static_cast<size_t>(msg.payloadLen), senderSlot);
        return true;
    }
    // v122 WIRE VERSION GATE -- at the TOP, before ANY identity side effect
    // (mirror install / guid / skin / prefs / nick store). Owned by
    // player_handshake_version.cpp: pure pre-pass + byte-equality on the peer's
    // game target (build number = the header's protocol version, equal by
    // construction); mismatch or malformed chain -> refuse-close (host Kick +
    // feed line / client Fail popup). A refused joiner == the already-handled
    // "connected, never Joined, disconnected" lifecycle. Covers EVERY entry
    // surface -- browser, direct connect, env boot.
    if (ValidateJoinVersionOrRefuse(session, senderSlot, msg.payload, msg.payloadLen))
        return true;
    // CLIENT: this Join is the HOST introducing itself, and it is where our row 0
    // is born. The host's player number is a ROLE CONSTANT, so a client knows it
    // without being told -- no wire field, no ordering dependency on the roster
    // pulse. (On the host, the sender's row was already born by the reconcile.)
    if (session.role() == net::Role::Client && senderSlot == 0) {
        coop::roster_ledger::InstallRow(0, coop::roster_ledger::kHostPlayerNo,
                                        /*bornGeneration=*/0);
    }
    uint32_t senderElementId = 0;
    std::memcpy(&senderElementId, msg.payload, 4);
    const uint8_t* nickStart = msg.payload + 4;
    size_t nickRemaining = msg.payloadLen - 4;
    std::wstring nick = coop::roster_ledger::Get(senderSlot).nick;
    size_t nickFieldLen = 0;  // bytes the nick field [u8 len][bytes] occupies (0 if malformed/absent)
    if (nickRemaining > 0) {
        const int len = nickStart[0];
        if (1 + len <= static_cast<int>(nickRemaining)) {
            nickFieldLen = 1 + static_cast<size_t>(len);
            if (len > 0) nick = FromUtf8(nickStart + 1, len);
        }
    }
    // v73 per-player inventory: the GUID follows the nick: [u8 guidlen][guid ASCII]. Tolerate
    // absence (a peer that sent none -> the row's guid stays empty = first-join/empty inventory).
    size_t guidFieldLen = 0;  // bytes the guid field occupies (0 if malformed/absent)
    if (nickFieldLen > 0 && nickFieldLen < nickRemaining) {
        const uint8_t* guidStart = nickStart + nickFieldLen;
        const size_t guidRemaining = nickRemaining - nickFieldLen;
        const int glen = guidStart[0];
        if (1 + glen <= static_cast<int>(guidRemaining)) {
            guidFieldLen = 1 + static_cast<size_t>(glen);
            if (glen > 0) {
                std::string guid(reinterpret_cast<const char*>(guidStart + 1), static_cast<size_t>(glen));
                // SECURITY: this GUID becomes a host filesystem path component (coop_players/<guid>.json).
                // VALIDATE to exactly 32 hex chars at the wire boundary (a peer's config could be
                // tampered to send "..\\..\\evil") -- reject -> leave empty -> first-join/empty-inventory
                // path, no host file written outside coop_players. (Adversarial-verify HIGH, 2026-06-14.)
                if (IsValidGuid(guid))
                    coop::roster_ledger::SetGuid(senderSlot, std::move(guid));
                else
                    UE_LOGW("handshake: slot %d sent a non-hex/oversize GUID (%d bytes) -- rejecting "
                            "(empty-inventory fallback)", senderSlot, glen);
            }
        }
    }
    // v93 skins: [u8 skinlen][skin ASCII] follows the guid. Tolerated absent (pre-v93
    // never reaches here -- ParseHeader rejects -- but a malformed field just leaves the
    // slot's skin empty = native kel until a SkinChange lands).
    size_t skinFieldLen = 0;
    if (guidFieldLen > 0 && nickFieldLen + guidFieldLen < nickRemaining) {
        std::string skin;
        skinFieldLen = ParseSkinField(nickStart + nickFieldLen + guidFieldLen,
                                      nickRemaining - nickFieldLen - guidFieldLen, &skin);
        if (skinFieldLen > 0 && !skin.empty()) {
            StoreSkinForSlot(senderSlot, std::move(skin));
        }
    }
    // v94 display prefs: [u8 flags] follows the skin field (bit0 = nameplate visible).
    // Tolerated absent (malformed upstream fields just leave the defaults = visible).
    if (skinFieldLen > 0 &&
        nickFieldLen + guidFieldLen + skinFieldLen < nickRemaining) {
        StorePrefsFlagsForSlot(senderSlot,
                               nickStart[nickFieldLen + guidFieldLen + skinFieldLen]);
        // v103 nick color (12f): [u8 has][r][g][b] follows the flags byte.
        const size_t colorOff = nickFieldLen + guidFieldLen + skinFieldLen + 1;
        if (colorOff < nickRemaining)
            ParseNickColorField(nickStart + colorOff, nickRemaining - colorOff,
                                senderSlot);
    }
    // Install mirror Player Element for this sender so future
    // ItemActivate/Weather/etc. packets bearing senderElementId
    // resolve via Registry::Get on this peer. 0 means "no Element
    // yet"; skip mirror install and fall back to senderPeerSlot
    // routing per the field's contract.
    //
    // PR-FOUNDATION-1 (2026-05-29): range-validate senderElementId
    // against the sender's role before installing. A peer whose
    // role is host MUST send a host-range eid; a peer whose role is
    // client MUST send a peer-range eid. A mismatch indicates a
    // forged Join (or relay-loop bug) and we drop the mirror install
    // rather than corrupt the per-slot Player Element mapping. The
    // 0-sentinel passthrough (boot/seed race) stays as before.
    if (senderElementId != 0u &&
        senderElementId != coop::element::kInvalidId) {
        const bool senderIsHost = (senderSlot == 0);
        if (!coop::element::Registry::IsAllowedSenderEid(
                senderIsHost, senderElementId)) {
            UE_LOGW("player_handshake: Join senderElementId=0x%08x out of "
                    "allowed %s range (senderSlot=%d) -- dropping mirror "
                    "install; nickname will still display",
                    senderElementId,
                    senderIsHost ? "host" : "peer",
                    senderSlot);
        } else {
            coop::players::Registry::Get().EstablishMirrorForSlot(
                static_cast<uint8_t>(senderSlot), senderElementId);
        }
    }
    // VT-inspired nickname sanitizer (2026-05-25, see
    // SanitizeNickname doc above). Trust-boundary defense: this
    // string CAME FROM A PEER over UDP and is going to land in
    // our ImGui nameplate + the chat feed. Without
    // sanitization a hostile peer could newline-inject our
    // widget text or insert a RLO unicode override to mirror
    // the rest of the nameplate. Length-cap to 20 wchars caps
    // the worst-case widget overflow.
    nick = SanitizeNickname(nick);
    // ARC B -- the host is the canonical namer. Two clients typing the same name
    // each believe they are unique and neither can see the other's choice at the
    // moment it is made, so uniqueness is decided by the one peer that sees every
    // name at once. The assignment reaches the named peer (and everyone else) on
    // the RosterRow below, whose nick sits in the FIXED PREFIX above the
    // `applyDeclared` gate precisely so it can.
    //
    // Not gated on "is this a duplicate": Assign is the identity function when
    // nothing collides, and running it unconditionally means there is ONE path a
    // name can take to a row rather than two that must agree.
    if (session.role() == net::Role::Host)
        nick = coop::nickname_arbiter::Assign(senderSlot, nick);
    coop::roster_ledger::SetNick(senderSlot, nick);
    // Label the nameplate of THIS sender's puppet (not all puppets).
    if (RemotePlayer* p = coop::players::Registry::Get().Puppet(
            static_cast<uint8_t>(senderSlot))) {
        p->SetNickname(nick);
    }
    // Two-phase, role-aware phrasing (2026-06-15, user feedback): the Join handshake means the
    // peer is CONNECTING -- it has not loaded the world / spawned yet. Announce "connecting" here;
    // the "<nick> joined the game" line fires later from net_pump when the peer's PUPPET actually
    // spawns (AnnouncePeerSpawned). On the CLIENT the Join arrives FROM the host, so phrase it from
    // the receiver's POV ("Connecting to <host>'s game", not "<host> is connecting").
    if (session.role() == net::Role::Client) {
        coop::chat_feed::Push(L"Connecting to " + nick + L"'s game...");
    } else {
        coop::chat_feed::Push(nick + L" is connecting to the game...");
    }
    // PR-FOUNDATION Tier 2 T2-1 (host-relay): if WE are the host, this Join
    // came from a client. Run the MTA InitialDataStream two-way cross-peer
    // identity broadcast so every client learns about this joiner AND the
    // joiner learns about every existing client. No-op on the client (it
    // never relays). Pass the validated eid + sanitized nick we just
    // resolved. Skipped when senderElementId is the 0 sentinel (the joiner
    // had no Element yet -- its retry Join will carry a real eid and
    // re-trigger this).
    if (session.role() == net::Role::Host &&
        senderElementId != 0u &&
        senderElementId != coop::element::kInvalidId) {
        BroadcastRosterFromHost(session, senderSlot, senderElementId, nick);
    }
    // Seen-players registry (F1 Administration): record this peer's durable
    // identity (guid + nick + IP + last-seen) on the host. After the guid/nick
    // stores above so TouchOnJoin reads the just-landed values.
    if (session.role() == net::Role::Host)
        coop::seen_players::TouchOnJoin(session, senderSlot);
    return true;
}

namespace {

// The single door for "<nick> joined the game" (any viewer role). Latched once
// per join (cleared on slot disconnect) so the two seams below -- puppet spawn
// and world-ready -- can both call it in either order without a repeat, and a
// mid-session puppet respawn stays silent. IMMEDIATE push: the line's whole
// point (user 2026-07-03) is to coincide with the body actually appearing.
void AnnounceJoinerOnce(int slot) {
    UE_ASSERT_GAME_THREAD("ledger row joinAnnounced (AnnounceJoinerOnce)");
    if (slot < 1 || slot >= net::kMaxPeers) return;  // slot 0 = host self; never "joins"
    if (coop::roster_ledger::Get(slot).joinAnnounced) return;
    coop::roster_ledger::SetJoinAnnounced(slot, true);
    coop::chat_feed::Push(NicknameForSlot(static_cast<uint8_t>(slot)) + L" joined the game");
    UE_LOGI("player_handshake: slot %d joined the game (announced at puppet appearance)", slot);
}

}  // namespace

void AnnouncePeerSpawned(net::Role role, int slot) {
    // Called from net_pump the moment a remote peer's PUPPET spawns -- the visible
    // "appearance" seam (user 2026-07-03: the join line must coincide with the body
    // showing up; the old ClientWorldReady+5s announce ran ~6 s BEFORE the puppet in
    // the measured live flow -- world-ready 15:27:23 vs spawn 15:27:34). On the HOST
    // net_pump gates this call on IsSlotWorldReady, preserving the 2026-06-17
    // protection against a pre-world menu pose spawning the puppet early; in that
    // order OnClientWorldReady (below) announces instead.
    if (role == net::Role::Client && slot == 0) {
        // Self-join line: own loading screen still covers the world at the spawn
        // moment, so an immediate line looks premature (user 2026-06-21) -- keep +5 s.
        coop::chat_feed::PushDelayed(L"Joined " + NicknameForSlot(0) + L"'s game", 5000);
        return;
    }
    AnnounceJoinerOnce(slot);
}

void OnClientWorldReady(int slot) {
    // HOST-side reverse-order cover: if this slot's puppet spawned BEFORE its
    // world-ready (a pre-world menu/loading pose -- the 2026-06-17 case), the
    // spawn-seam announce was skipped; the body is standing here already, and
    // world-ready is the moment it becomes the real joiner. Normal flow (spawn
    // after world-ready) leaves this a no-op and the spawn seam announces.
    if (slot < 1 || slot >= net::kMaxPeers) return;
    if (coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot)) != nullptr)
        AnnounceJoinerOnce(slot);
}

bool HandleAssignPeerSlot(net::Session& session,
                          const net::Session::ReliableMessage& msg) {
    // Host tells us which peer slot we were assigned. Without
    // this the client would self-stamp LocalPeerId=1 from a
    // hardcoded 1v1 mapping, and a second client with the same
    // local ID would silently self-echo-drop the first's
    // ItemActivate as a "loopback bounce" (see event_feed's
    // ItemActivate self-echo guard).
    if (msg.payloadLen < sizeof(net::AssignPeerSlotPayload)) {
        UE_LOGW("player_handshake: AssignPeerSlot payload too short (%zu < %zu)",
                static_cast<size_t>(msg.payloadLen), sizeof(net::AssignPeerSlotPayload));
        return true;
    }
    net::AssignPeerSlotPayload p{};
    std::memcpy(&p, msg.payload, sizeof(p));
    // Trust boundary: only the host sends this; reject on host.
    if (session.role() == net::Role::Host) {
        UE_LOGW("player_handshake: AssignPeerSlot received on host -- dropping "
                "(host self-assigns slot 0; no inbound from client)");
        return true;
    }
    // Enforce that the SENDER connection is the host (senderPeerSlot == 0).
    // A malicious client peer could otherwise craft AssignPeerSlot packets
    // if GNS ever fans out client-to-client traffic; defending here
    // independent of relay topology.
    if (msg.senderPeerSlot != 0) {
        UE_LOGW("player_handshake: AssignPeerSlot from non-host "
                "senderPeerSlot=%d -- dropping",
                msg.senderPeerSlot);
        return true;
    }
    // Slot must be a valid CLIENT slot (1..kMaxPeers-1). Slot 0 is
    // the host's reserved local-self slot.
    if (p.slot < 1 || p.slot >= net::kMaxPeers) {
        UE_LOGW("player_handshake: AssignPeerSlot slot=%u out of range [1..%u) -- dropping",
                p.slot, static_cast<unsigned>(net::kMaxPeers));
        return true;
    }
    coop::players::Registry::Get().SetLocalPeerId(p.slot);
    UE_LOGI("player_handshake: host assigned us peer slot %u (Registry::LocalPeerId now %u)",
            p.slot, coop::players::Registry::Get().LocalPeerId());
    // Any roster row that arrived before this stamp was PARKED -- until now a row
    // about our own slot was indistinguishable from a row about a remote peer.
    // Apply them in arrival order now that the ambiguity is gone.
    OnLocalPeerIdStamped(session);
    // v13 (A4 2026-05-29): if the host included its Player Element
    // id, install a mirror in slot 0 so subsequent wire packets
    // carrying host's senderElementId resolve via Registry::Get on
    // this client. hostElementId == 0 or kInvalidId means the host
    // hadn't allocated its Element yet -- skip mirror install; the
    // receivers will fall back to senderPeerSlot routing. v16
    // (PR-FOUNDATION-1b): the hostContext byte v14 added is gone;
    // mirror install no longer carries any generation byte.
    if (p.hostElementId != 0u &&
        p.hostElementId != coop::element::kInvalidId) {
        // PR-FOUNDATION-1 (2026-05-29): use the canonical helper.
        // IsAllowedHostAllocatedEid additionally rejects id==0 and
        // kInvalidId which the outer if-guard above already filters,
        // so behavior is unchanged -- this is a uniform call-site
        // refactor so every receiver routes through one validator.
        if (!coop::element::Registry::IsAllowedHostAllocatedEid(p.hostElementId)) {
            UE_LOGW("player_handshake: AssignPeerSlot hostElementId=0x%08x is "
                    "not in host range -- dropping mirror install",
                    p.hostElementId);
        } else {
            coop::players::Registry::Get().EstablishMirrorForSlot(
                coop::players::kPeerIdHost, p.hostElementId);
        }
    } else {
        UE_LOGI("player_handshake: AssignPeerSlot host had no Element id yet "
                "(boot/seed race) -- routing will use senderPeerSlot");
    }
    return true;
}

// The live display-pref change family (SkinChange / NameplateChange /
// NickColorChange announce + handle) lives in player_handshake_prefs.cpp
// (extracted 2026-07-05, modular file-size rule). Shared internals:
// player_handshake_detail.h.

}  // namespace coop::player_handshake
