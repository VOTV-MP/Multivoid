// coop/player_handshake.cpp -- see coop/player_handshake.h.

#include "coop/session/player_handshake.h"

#include "coop/session/seen_players.h"

#include "coop/element/player.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/local_body.h"
#include "coop/player/nameplate.h"
#include "coop/player/nick_color.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/skin_registry.h"
#include "ue_wrap/hot_path_guard.h"
#include "coop/comms/chat_feed.h"
#include "ue_wrap/log.h"

#include <windows.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace coop::player_handshake {

namespace {

std::wstring g_localNick = L"Player";
// Per-slot nickname + per-slot Join-sent latch. Single-peer scalars
// would let two clients' Joins overwrite each other and a reconnect
// would skip the re-announce.
std::array<std::wstring, net::kMaxPeers> g_remoteNickBySlot{
    L"Remote player", L"Remote player", L"Remote player", L"Remote player"
};
std::array<bool, net::kMaxPeers> g_joinSentBySlot{};
// Once-per-join latch for the "<nick> joined the game" line (fired at the joiner's PUPPET
// APPEARANCE -- see AnnounceJoinerOnce; the spawn seam and the world-ready seam both funnel
// through it). Cleared on slot disconnect so a reconnect re-announces.
std::array<bool, net::kMaxPeers> g_joinAnnouncedBySlot{};

// v73 per-player inventory identity. g_localGuid rides our outbound Join; g_guidBySlot[s]
// caches the GUID peer `s` sent (HOST-side, keys coop_players/<guid>.json). ASCII 32-hex.
std::string g_localGuid;
std::array<std::string, net::kMaxPeers> g_guidBySlot{};

// v93 skins: the body-skin name each peer announced (Join field / SkinChange).
// Empty = not yet known -> that slot's puppet spawns native kel and is
// re-skinned the moment the name lands (StoreSkinForSlot).
std::array<std::string, net::kMaxPeers> g_skinBySlot{};

// Store + live-apply: if the described slot's puppet is already spawned, re-skin
// it NOW (mid-session SkinChange, or a Join that raced the first pose).
void StoreSkinForSlot(int slot, std::string name) {
    if (slot < 0 || slot >= net::kMaxPeers) return;
    if (g_skinBySlot[slot] == name) return;
    g_skinBySlot[slot] = std::move(name);
    if (RemotePlayer* p = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot)))
        p->ApplySkin(g_skinBySlot[slot]);
}

// v94 display-prefs flags byte (appended to Join + PlayerJoined after the skin
// field). Bit layout is the wire contract -- extend with new bits, never re-order.
constexpr uint8_t kPrefNameplateVisible = 0x01;

uint8_t BuildLocalPrefsFlags() {
    uint8_t f = 0;
    if (coop::nameplate::LocalVisible()) f |= kPrefNameplateVisible;
    return f;
}

uint8_t PrefsFlagsForSlot(int slot) {
    uint8_t f = 0;
    if (coop::nameplate::VisibleForSlot(slot)) f |= kPrefNameplateVisible;
    return f;
}

void StorePrefsFlagsForSlot(int slot, uint8_t flags) {
    coop::nameplate::StoreVisibleForSlot(slot, (flags & kPrefNameplateVisible) != 0);
}

// v103 nick color (12f): a self-describing [u8 has][u8 r][u8 g][u8 b] field
// appended to Join + PlayerJoined after the prefs flags byte. has=0 -> the 3
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

// Parse one [u8 len][ASCII] field. Returns bytes consumed (0 = malformed/absent);
// `out` untouched unless a well-formed non-empty field validated as a skin name.
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

std::vector<uint8_t> ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::vector<uint8_t> out(n > 0 ? n : 0);
    if (n > 0)
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                              reinterpret_cast<char*>(out.data()), n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const uint8_t* p, int len) {
    if (len <= 0) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p), len, nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p), len, out.data(), n);
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
constexpr size_t kMaxNickLen = 20;
std::wstring SanitizeNickname(const std::wstring& raw) {
    std::wstring out;
    out.reserve(raw.size());
    bool lastWasSpace = true;  // primes the leading-space trim
    for (wchar_t c : raw) {
        const bool isAlnum = (c >= L'0' && c <= L'9') ||
                             (c >= L'A' && c <= L'Z') ||
                             (c >= L'a' && c <= L'z');
        const bool isSafePunct = (c == L'-' || c == L'_' || c == L'.');
        const bool isSpace = (c == L' ');
        if (isAlnum || isSafePunct) {
            out.push_back(c);
            lastWasSpace = false;
        } else if (isSpace && !lastWasSpace) {
            out.push_back(L' ');
            lastWasSpace = true;
        }
        // else: strip silently (control chars, unicode, punctuation, etc.)
        if (out.size() >= kMaxNickLen) break;
    }
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

}  // namespace

void SetLocalNickname(const std::wstring& nick) {
    // VT-inspired sanitize-on-input (2026-05-25): symmetric defense.
    // Sanitizing here too means our env-var setup (VOTVCOOP_NET_NICK)
    // can't accidentally send garbage over the wire that we then
    // sanitize on the OTHER end -- net is cleaner if both ends agree
    // on the displayable form.
    if (!nick.empty()) g_localNick = SanitizeNickname(nick);
}

const std::wstring& LocalNickname() { return g_localNick; }

void SetLocalGuid(const std::string& guid) { g_localGuid = guid; }

const std::string& GuidForSlot(int slot) {
    UE_ASSERT_GAME_THREAD("g_guidBySlot (GuidForSlot)");
    static const std::string kEmpty;
    if (slot < 0 || slot >= net::kMaxPeers) return kEmpty;
    return g_guidBySlot[slot];
}

const std::string& SkinForSlot(int slot) {
    UE_ASSERT_GAME_THREAD("g_skinBySlot (SkinForSlot)");
    static const std::string kEmpty;
    if (slot < 0 || slot >= net::kMaxPeers) return kEmpty;
    return g_skinBySlot[slot];
}

void Reset() {
    for (auto& nick : g_remoteNickBySlot) nick = L"Remote player";
    for (auto& g : g_guidBySlot) g.clear();
    for (auto& s : g_skinBySlot) s.clear();
    coop::nameplate::ResetSlots();  // v94: per-slot plate prefs back to visible
    coop::nick_color::ResetSlots();  // v103: per-slot nick colors back to default
    g_joinSentBySlot.fill(false);
    g_joinAnnouncedBySlot.fill(false);
}

void MaybeSendJoinToSlot(net::Session& session, int slot,
                         std::vector<uint8_t>& joinPayload,
                         bool& joinPayloadBuilt) {
    // Reads g_localNick + g_joinSentBySlot (GT-only side-tables, T-8). Called
    // from the net_pump connect-edge each tick. (The boot-thread WRITER
    // SetLocalNickname stays unguarded -- it runs once on the bringup thread
    // before the pump sends any Join.)
    UE_ASSERT_GAME_THREAD("g_joinSentBySlot/g_localNick (MaybeSendJoinToSlot)");
    if (slot < 0 || slot >= net::kMaxPeers) return;
    if (g_joinSentBySlot[slot]) return;
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
        std::vector<uint8_t> nickUtf8 = ToUtf8(g_localNick);
        if (nickUtf8.size() > 200) nickUtf8.resize(200);
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
        joinPayloadBuilt = true;
    }
    if (session.SendReliableToSlot(slot, net::ReliableKind::Join,
                                   joinPayload.data(),
                                   static_cast<int>(joinPayload.size()))) {
        g_joinSentBySlot[slot] = true;
    }
    // If send fails (transient), the caller will hit this slot again next tick.
}

void OnSlotDisconnected(int slot) {
    // Clears g_joinSentBySlot + g_remoteNickBySlot (T-8); fired from the
    // net_pump disconnect edge (game thread).
    UE_ASSERT_GAME_THREAD("g_remoteNickBySlot (OnSlotDisconnected)");
    if (slot < 0 || slot >= net::kMaxPeers) return;
    g_joinSentBySlot[slot] = false;
    g_joinAnnouncedBySlot[slot] = false;  // a reconnect on this slot re-announces "joined" on its next ClientWorldReady
    // N-5 (2026-05-29 audit): clear the stashed nickname so a subsequent peer
    // reusing the same slot before its Join arrives doesn't display the
    // departed peer's name in the HUD toasts. NicknameForSlot falls back to
    // a placeholder when this is empty.
    g_remoteNickBySlot[slot].clear();
    g_guidBySlot[slot].clear();  // v73: drop the departed peer's inventory GUID for this slot
    g_skinBySlot[slot].clear();  // v93: a reconnect (or a different peer on this slot) re-announces its skin
    coop::nameplate::OnSlotDisconnected(slot);  // v94: plate pref back to visible for a slot reuse
    coop::nick_color::OnSlotDisconnected(slot);  // v103: nick color back to default for a slot reuse
}

const std::wstring& NicknameForSlot(int slot) {
    // Reads g_remoteNickBySlot (T-8). Callers (puppet-spawn nameplate labelling
    // in net_pump, the handshake handlers) are all on the game thread.
    UE_ASSERT_GAME_THREAD("g_remoteNickBySlot (NicknameForSlot)");
    static const std::wstring kPlaceholder = L"Remote player";
    if (slot < 0 || slot >= net::kMaxPeers) return kPlaceholder;
    return g_remoteNickBySlot[slot];
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
    // Reads + writes g_remoteNickBySlot (T-8). Dispatched from
    // event_feed::Update on the game thread (net_pump::Tick -> Update).
    UE_ASSERT_GAME_THREAD("g_remoteNickBySlot (HandleJoinMessage)");
    // Per-slot nickname keyed on the sender's peer slot. A single
    // global scalar would let two clients on host clobber each
    // other's nick on every Join.
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
    uint32_t senderElementId = 0;
    std::memcpy(&senderElementId, msg.payload, 4);
    const uint8_t* nickStart = msg.payload + 4;
    size_t nickRemaining = msg.payloadLen - 4;
    std::wstring nick = g_remoteNickBySlot[senderSlot];
    size_t nickFieldLen = 0;  // bytes the nick field [u8 len][bytes] occupies (0 if malformed/absent)
    if (nickRemaining > 0) {
        const int len = nickStart[0];
        if (1 + len <= static_cast<int>(nickRemaining)) {
            nickFieldLen = 1 + static_cast<size_t>(len);
            if (len > 0) nick = FromUtf8(nickStart + 1, len);
        }
    }
    // v73 per-player inventory: the GUID follows the nick: [u8 guidlen][guid ASCII]. Tolerate
    // absence (a peer that sent none -> g_guidBySlot stays empty = first-join/empty inventory).
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
                    g_guidBySlot[senderSlot] = std::move(guid);
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
    g_remoteNickBySlot[senderSlot] = nick;
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
        BroadcastPlayerJoinedFromHost(session, senderSlot, senderElementId, nick);
    }
    // Seen-players registry (F1 Administration): record this peer's durable
    // identity (guid + nick + IP + last-seen) on the host. After the guid/nick
    // stores above so TouchOnJoin reads the just-landed values.
    if (session.role() == net::Role::Host)
        coop::seen_players::TouchOnJoin(session, senderSlot);
    return true;
}

namespace {

// Build a PlayerJoined reliable payload describing peer `slot` (its eid +
// nick + v93 skin + v94 prefs flags + v103 nick color). Wire layout (parsed
// field-by-field, same as Join):
//   [uint8 slot][uint32 eid][uint8 nicklen][nick UTF-8][uint8 skinlen][skin ASCII]
//   [uint8 flags][u8 hasColor][u8 r][u8 g][u8 b]
std::vector<uint8_t> BuildPlayerJoinedPayload(uint8_t slot, uint32_t eid,
                                              const std::wstring& nick,
                                              const std::string& skin,
                                              uint8_t prefsFlags) {
    std::vector<uint8_t> out;
    out.resize(5);
    out[0] = slot;
    std::memcpy(out.data() + 1, &eid, 4);
    std::vector<uint8_t> nickUtf8 = ToUtf8(nick);
    if (nickUtf8.size() > 200) nickUtf8.resize(200);
    out.push_back(static_cast<uint8_t>(nickUtf8.size()));
    out.insert(out.end(), nickUtf8.begin(), nickUtf8.end());
    const uint8_t skinLen = static_cast<uint8_t>(skin.size() > 48 ? 48 : skin.size());
    out.push_back(skinLen);
    out.insert(out.end(), skin.begin(), skin.begin() + skinLen);
    out.push_back(prefsFlags);
    AppendNickColorField(out, coop::nick_color::PackedForSlot(slot));
    return out;
}

}  // namespace

void BroadcastPlayerJoinedFromHost(net::Session& session, int joinerSlot,
                                   uint32_t joinerEid,
                                   const std::wstring& joinerNick) {
    if (session.role() != net::Role::Host) return;
    if (joinerSlot < 1 || joinerSlot >= net::kMaxPeers) return;

    auto& reg = coop::players::Registry::Get();

    // (1) Announce the joiner to every OTHER connected client. MTA:
    // CGame.cpp:1422-1426 BroadcastOnlyJoined(PlayerNotice, &Player).
    {
        const std::vector<uint8_t> p =
            BuildPlayerJoinedPayload(static_cast<uint8_t>(joinerSlot),
                                     joinerEid, joinerNick, SkinForSlot(joinerSlot),
                                     PrefsFlagsForSlot(joinerSlot));
        for (int x = 1; x < net::kMaxPeers; ++x) {
            if (x == joinerSlot) continue;
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, net::ReliableKind::PlayerJoined,
                                       p.data(), static_cast<int>(p.size()));
        }
    }

    // (2) Tell the joiner about every already-known client. MTA:
    // CGame.cpp:1435-1455 (build a PlayerList of all existing peers, send
    // to the joiner). "Known" = the host has installed that peer's MIRROR
    // Player Element (its Join was processed, so the cross-peer eid the
    // peer minted is authoritative). A connected-but-not-yet-joined peer
    // is skipped here; when ITS Join lands, step (1) of that Join announces
    // it to this joiner. Order-independent + symmetric.
    for (int x = 1; x < net::kMaxPeers; ++x) {
        if (x == joinerSlot) continue;
        if (!session.IsSlotReady(x)) continue;
        coop::element::Player* el = reg.GetPlayerElement(static_cast<uint8_t>(x));
        if (!el || !el->IsMirror()) continue;  // identity not yet known
        const std::vector<uint8_t> p =
            BuildPlayerJoinedPayload(static_cast<uint8_t>(x), el->GetId(),
                                     NicknameForSlot(x), SkinForSlot(x),
                                     PrefsFlagsForSlot(x));
        session.SendReliableToSlot(joinerSlot, net::ReliableKind::PlayerJoined,
                                   p.data(), static_cast<int>(p.size()));
    }
    UE_LOGI("player_handshake: host relayed PlayerJoined cross-peer identity "
            "for joiner slot %d (eid=0x%08x)", joinerSlot, joinerEid);
}

bool HandlePlayerJoined(net::Session& session,
                        const net::Session::ReliableMessage& msg) {
    // Reads + writes g_remoteNickBySlot (T-8). Dispatched from
    // event_feed::Update on the game thread (client-side cross-peer identity).
    UE_ASSERT_GAME_THREAD("g_remoteNickBySlot (HandlePlayerJoined)");
    // Host originates PlayerJoined; it must never receive one. Reject on
    // host + require the sender to be the host (senderPeerSlot==0) on the
    // client -- a client peer crafting PlayerJoined could otherwise inject
    // a forged peer identity.
    if (session.role() == net::Role::Host) {
        UE_LOGW("player_handshake: PlayerJoined received on host -- dropping");
        return true;
    }
    if (msg.senderPeerSlot != 0) {
        UE_LOGW("player_handshake: PlayerJoined from non-host senderPeerSlot=%d "
                "-- dropping", msg.senderPeerSlot);
        return true;
    }
    // Layout: [uint8 slot][uint32 eid][uint8 nicklen][nick UTF-8]. Minimum
    // is 6 bytes (slot + eid + nicklen, empty nick).
    if (msg.payloadLen < 6) {
        UE_LOGW("player_handshake: PlayerJoined payload %zu B too short -- dropping",
                static_cast<size_t>(msg.payloadLen));
        return true;
    }
    const uint8_t describedSlot = msg.payload[0];
    uint32_t describedEid = 0;
    std::memcpy(&describedEid, msg.payload + 1, 4);
    const uint8_t* nickStart = msg.payload + 5;
    const size_t nickRemaining = msg.payloadLen - 5;

    // The described peer must be a real client slot, and never OUR own slot
    // (the host should never describe us to ourselves) nor the host slot 0
    // (the host is delivered via AssignPeerSlot, not PlayerJoined).
    if (describedSlot < 1 || describedSlot >= net::kMaxPeers) {
        UE_LOGW("player_handshake: PlayerJoined slot=%u out of range -- dropping",
                static_cast<unsigned>(describedSlot));
        return true;
    }
    if (describedSlot == coop::players::Registry::Get().LocalPeerId()) {
        UE_LOGW("player_handshake: PlayerJoined describes our own slot %u "
                "-- dropping", static_cast<unsigned>(describedSlot));
        return true;
    }

    // The described peer is a CLIENT, so its eid must be in the peer range.
    // (Cross-peer eids are unique per slot thanks to the T2-0 banding.)
    if (!coop::element::Registry::IsAllowedPeerAllocatedEid(describedEid)) {
        UE_LOGW("player_handshake: PlayerJoined slot=%u eid=0x%08x not in peer "
                "range -- dropping mirror install", static_cast<unsigned>(describedSlot),
                describedEid);
        return true;
    }

    // Install the peer's mirror Player Element so a subsequently-relayed
    // pose/ItemActivate bearing describedEid resolves via Registry::Get.
    coop::players::Registry::Get().EstablishMirrorForSlot(describedSlot, describedEid);

    // Cache + sanitize the nick. If the puppet for this slot is already
    // spawned (a relayed pose beat the relayed identity), label it now;
    // otherwise SetNickname runs when the puppet spawns + the cached nick
    // is read. Mirrors HandleJoinMessage's nickname handling.
    std::wstring nick = g_remoteNickBySlot[describedSlot];
    size_t nickFieldLen = 0;
    if (nickRemaining > 0) {
        const int len = nickStart[0];
        if (1 + len <= static_cast<int>(nickRemaining)) {
            nickFieldLen = 1 + static_cast<size_t>(len);
            if (len > 0) nick = FromUtf8(nickStart + 1, len);
        }
    }
    // v93 skins: [u8 skinlen][skin] follows the nick (host-relayed identity).
    size_t skinFieldLen = 0;
    if (nickFieldLen > 0 && nickFieldLen < nickRemaining) {
        std::string skin;
        skinFieldLen = ParseSkinField(nickStart + nickFieldLen,
                                      nickRemaining - nickFieldLen, &skin);
        if (skinFieldLen > 0 && !skin.empty()) {
            StoreSkinForSlot(describedSlot, std::move(skin));
        }
    }
    // v94 display prefs: [u8 flags] follows the skin field.
    if (skinFieldLen > 0 && nickFieldLen + skinFieldLen < nickRemaining) {
        StorePrefsFlagsForSlot(describedSlot, nickStart[nickFieldLen + skinFieldLen]);
        // v103 nick color (12f): [u8 has][r][g][b] follows the flags byte.
        const size_t colorOff = nickFieldLen + skinFieldLen + 1;
        if (colorOff < nickRemaining)
            ParseNickColorField(nickStart + colorOff, nickRemaining - colorOff,
                                describedSlot);
    }
    nick = SanitizeNickname(nick);
    g_remoteNickBySlot[describedSlot] = nick;
    if (RemotePlayer* p = coop::players::Registry::Get().Puppet(describedSlot)) {
        p->SetNickname(nick);
    }
    // Cross-peer: another client is CONNECTING (its puppet spawns later -> AnnouncePeerSpawned
    // fires the "joined the game" line then).
    coop::chat_feed::Push(nick + L" is connecting to the game...");
    UE_LOGI("player_handshake: client installed cross-peer identity slot=%u "
            "eid=0x%08x nick='%ls'", static_cast<unsigned>(describedSlot),
            describedEid, nick.c_str());
    return true;
}

namespace {

// The single door for "<nick> joined the game" (any viewer role). Latched once
// per join (cleared on slot disconnect) so the two seams below -- puppet spawn
// and world-ready -- can both call it in either order without a repeat, and a
// mid-session puppet respawn stays silent. IMMEDIATE push: the line's whole
// point (user 2026-07-03) is to coincide with the body actually appearing.
void AnnounceJoinerOnce(int slot) {
    UE_ASSERT_GAME_THREAD("g_joinAnnouncedBySlot (AnnounceJoinerOnce)");
    if (slot < 1 || slot >= net::kMaxPeers) return;  // slot 0 = host self; never "joins"
    if (g_joinAnnouncedBySlot[slot]) return;
    g_joinAnnouncedBySlot[slot] = true;
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

namespace {

// [u8 slot][u8 len][name ASCII] -- the SkinChange wire form (also what the host
// rebroadcasts verbatim).
std::vector<uint8_t> BuildSkinChangePayload(uint8_t slot, const std::string& name) {
    std::vector<uint8_t> out;
    const uint8_t len = static_cast<uint8_t>(name.size() > 48 ? 48 : name.size());
    out.reserve(2 + len);
    out.push_back(slot);
    out.push_back(len);
    out.insert(out.end(), name.begin(), name.begin() + len);
    return out;
}

}  // namespace

void AnnounceLocalSkin(net::Session& session, const std::string& name) {
    UE_ASSERT_GAME_THREAD("AnnounceLocalSkin");
    if (!coop::skins::IsValidSkinName(name)) return;
    const uint8_t selfSlot = coop::players::Registry::Get().LocalPeerId();
    if (selfSlot >= net::kMaxPeers) return;  // not in a session yet -- the Join will carry it
    const std::vector<uint8_t> p = BuildSkinChangePayload(selfSlot, name);
    if (session.role() == net::Role::Host) {
        for (int x = 1; x < net::kMaxPeers; ++x) {
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, net::ReliableKind::SkinChange,
                                       p.data(), static_cast<int>(p.size()));
        }
    } else {
        session.SendReliableToSlot(0, net::ReliableKind::SkinChange,
                                   p.data(), static_cast<int>(p.size()));
    }
    UE_LOGI("player_handshake: announced local skin '%s' (slot %u)", name.c_str(),
            static_cast<unsigned>(selfSlot));
}

bool HandleSkinChange(net::Session& session,
                      const net::Session::ReliableMessage& msg) {
    UE_ASSERT_GAME_THREAD("g_skinBySlot (HandleSkinChange)");
    if (msg.payloadLen < 2) {
        UE_LOGW("player_handshake: SkinChange payload %zu B too short -- dropping",
                static_cast<size_t>(msg.payloadLen));
        return true;
    }
    const uint8_t describedSlot = msg.payload[0];
    std::string name;
    if (ParseSkinField(msg.payload + 1, static_cast<size_t>(msg.payloadLen) - 1, &name) == 0 ||
        name.empty()) {
        UE_LOGW("player_handshake: SkinChange malformed/invalid name from slot %d -- dropping",
                msg.senderPeerSlot);
        return true;
    }
    if (describedSlot >= net::kMaxPeers) return true;

    if (session.role() == net::Role::Host) {
        // Forgery guard: a client may only change ITS OWN skin.
        if (msg.senderPeerSlot != describedSlot || describedSlot == 0) {
            UE_LOGW("player_handshake: SkinChange slot=%u from senderSlot=%d -- forged, dropping",
                    static_cast<unsigned>(describedSlot), msg.senderPeerSlot);
            return true;
        }
        StoreSkinForSlot(describedSlot, name);
        coop::chat_feed::Push(NicknameForSlot(describedSlot) + L" changed skin to " +
                              std::wstring(name.begin(), name.end()));
        // Rebroadcast to every other ready client (originator excluded).
        const std::vector<uint8_t> p = BuildSkinChangePayload(describedSlot, name);
        for (int x = 1; x < net::kMaxPeers; ++x) {
            if (x == describedSlot) continue;
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, net::ReliableKind::SkinChange,
                                       p.data(), static_cast<int>(p.size()));
        }
        return true;
    }

    // Client: only the host relays skin state.
    if (msg.senderPeerSlot != 0) {
        UE_LOGW("player_handshake: SkinChange from non-host senderPeerSlot=%d -- dropping",
                msg.senderPeerSlot);
        return true;
    }
    if (describedSlot == coop::players::Registry::Get().LocalPeerId()) return true;  // our own echo
    StoreSkinForSlot(describedSlot, name);
    coop::chat_feed::Push(NicknameForSlot(describedSlot) + L" changed skin to " +
                          std::wstring(name.begin(), name.end()));
    return true;
}

namespace {

// [u8 slot][u8 visible] -- the NameplateChange wire form (host rebroadcasts verbatim).
std::vector<uint8_t> BuildNameplateChangePayload(uint8_t slot, bool visible) {
    return { slot, static_cast<uint8_t>(visible ? 1 : 0) };
}

}  // namespace

void AnnounceLocalNameplate(net::Session& session, bool visible) {
    UE_ASSERT_GAME_THREAD("AnnounceLocalNameplate");
    const uint8_t selfSlot = coop::players::Registry::Get().LocalPeerId();
    if (selfSlot >= net::kMaxPeers) return;  // not in a session yet -- the Join will carry it
    const std::vector<uint8_t> p = BuildNameplateChangePayload(selfSlot, visible);
    if (session.role() == net::Role::Host) {
        for (int x = 1; x < net::kMaxPeers; ++x) {
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, net::ReliableKind::NameplateChange,
                                       p.data(), static_cast<int>(p.size()));
        }
    } else {
        session.SendReliableToSlot(0, net::ReliableKind::NameplateChange,
                                   p.data(), static_cast<int>(p.size()));
    }
    UE_LOGI("player_handshake: announced local nameplate %s (slot %u)",
            visible ? "VISIBLE" : "HIDDEN", static_cast<unsigned>(selfSlot));
}

bool HandleNameplateChange(net::Session& session,
                           const net::Session::ReliableMessage& msg) {
    UE_ASSERT_GAME_THREAD("HandleNameplateChange");
    if (msg.payloadLen < 2) {
        UE_LOGW("player_handshake: NameplateChange payload %zu B too short -- dropping",
                static_cast<size_t>(msg.payloadLen));
        return true;
    }
    const uint8_t describedSlot = msg.payload[0];
    const bool visible = msg.payload[1] != 0;
    if (describedSlot >= net::kMaxPeers) return true;

    if (session.role() == net::Role::Host) {
        // Forgery guard: a client may only toggle ITS OWN plate.
        if (msg.senderPeerSlot != describedSlot || describedSlot == 0) {
            UE_LOGW("player_handshake: NameplateChange slot=%u from senderSlot=%d -- forged, dropping",
                    static_cast<unsigned>(describedSlot), msg.senderPeerSlot);
            return true;
        }
        coop::nameplate::StoreVisibleForSlot(describedSlot, visible);
        // Rebroadcast to every other ready client (originator excluded).
        const std::vector<uint8_t> p = BuildNameplateChangePayload(describedSlot, visible);
        for (int x = 1; x < net::kMaxPeers; ++x) {
            if (x == describedSlot) continue;
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, net::ReliableKind::NameplateChange,
                                       p.data(), static_cast<int>(p.size()));
        }
        return true;
    }

    // Client: only the host relays plate prefs.
    if (msg.senderPeerSlot != 0) {
        UE_LOGW("player_handshake: NameplateChange from non-host senderPeerSlot=%d -- dropping",
                msg.senderPeerSlot);
        return true;
    }
    if (describedSlot == coop::players::Registry::Get().LocalPeerId()) return true;  // our own echo
    coop::nameplate::StoreVisibleForSlot(describedSlot, visible);
    return true;
}

namespace {

// [u8 slot][u8 has][u8 r][u8 g][u8 b] -- the NickColorChange wire form (host
// rebroadcasts verbatim).
std::vector<uint8_t> BuildNickColorChangePayload(uint8_t slot, uint32_t packed) {
    return { slot, static_cast<uint8_t>(coop::nick_color::IsCustom(packed) ? 1 : 0),
             coop::nick_color::R(packed), coop::nick_color::G(packed),
             coop::nick_color::B(packed) };
}

}  // namespace

void AnnounceLocalNickColor(net::Session& session, uint32_t packed) {
    UE_ASSERT_GAME_THREAD("AnnounceLocalNickColor");
    const uint8_t selfSlot = coop::players::Registry::Get().LocalPeerId();
    if (selfSlot >= net::kMaxPeers) return;  // not in a session yet -- the Join will carry it
    const std::vector<uint8_t> p = BuildNickColorChangePayload(selfSlot, packed);
    if (session.role() == net::Role::Host) {
        for (int x = 1; x < net::kMaxPeers; ++x) {
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, net::ReliableKind::NickColorChange,
                                       p.data(), static_cast<int>(p.size()));
        }
    } else {
        session.SendReliableToSlot(0, net::ReliableKind::NickColorChange,
                                   p.data(), static_cast<int>(p.size()));
    }
    UE_LOGI("player_handshake: announced local nick color %s (slot %u)",
            coop::nick_color::IsCustom(packed) ? "CUSTOM" : "DEFAULT",
            static_cast<unsigned>(selfSlot));
}

bool HandleNickColorChange(net::Session& session,
                           const net::Session::ReliableMessage& msg) {
    UE_ASSERT_GAME_THREAD("HandleNickColorChange");
    if (msg.payloadLen < 5) {
        UE_LOGW("player_handshake: NickColorChange payload %zu B too short -- dropping",
                static_cast<size_t>(msg.payloadLen));
        return true;
    }
    const uint8_t describedSlot = msg.payload[0];
    const uint32_t packed = msg.payload[1] != 0
        ? coop::nick_color::Pack(msg.payload[2], msg.payload[3], msg.payload[4])
        : 0u;
    if (describedSlot >= net::kMaxPeers) return true;

    if (session.role() == net::Role::Host) {
        // Forgery guard: a client may only color ITS OWN nick.
        if (msg.senderPeerSlot != describedSlot || describedSlot == 0) {
            UE_LOGW("player_handshake: NickColorChange slot=%u from senderSlot=%d -- forged, dropping",
                    static_cast<unsigned>(describedSlot), msg.senderPeerSlot);
            return true;
        }
        coop::nick_color::StoreForSlot(describedSlot, packed);
        // Rebroadcast to every other ready client (originator excluded).
        const std::vector<uint8_t> p = BuildNickColorChangePayload(describedSlot, packed);
        for (int x = 1; x < net::kMaxPeers; ++x) {
            if (x == describedSlot) continue;
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, net::ReliableKind::NickColorChange,
                                       p.data(), static_cast<int>(p.size()));
        }
        return true;
    }

    // Client: only the host relays color prefs.
    if (msg.senderPeerSlot != 0) {
        UE_LOGW("player_handshake: NickColorChange from non-host senderPeerSlot=%d -- dropping",
                msg.senderPeerSlot);
        return true;
    }
    if (describedSlot == coop::players::Registry::Get().LocalPeerId()) return true;  // our own echo
    coop::nick_color::StoreForSlot(describedSlot, packed);
    return true;
}

}  // namespace coop::player_handshake
