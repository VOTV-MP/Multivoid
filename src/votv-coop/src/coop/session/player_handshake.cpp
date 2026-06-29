// coop/player_handshake.cpp -- see coop/player_handshake.h.

#include "coop/session/player_handshake.h"

#include "coop/element/player.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
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
// HOST-side once-per-join latch for the "<nick> joined the game" line (fired on ClientWorldReady, not
// on the first pose -- see OnClientWorldReady). Cleared on slot disconnect so a reconnect re-announces.
std::array<bool, net::kMaxPeers> g_joinAnnouncedBySlot{};

// v73 per-player inventory identity. g_localGuid rides our outbound Join; g_guidBySlot[s]
// caches the GUID peer `s` sent (HOST-side, keys coop_players/<guid>.json). ASCII 32-hex.
std::string g_localGuid;
std::array<std::string, net::kMaxPeers> g_guidBySlot{};

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

void Reset() {
    for (auto& nick : g_remoteNickBySlot) nick = L"Remote player";
    for (auto& g : g_guidBySlot) g.clear();
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
    if (nickFieldLen > 0 && nickFieldLen < nickRemaining) {
        const uint8_t* guidStart = nickStart + nickFieldLen;
        const size_t guidRemaining = nickRemaining - nickFieldLen;
        const int glen = guidStart[0];
        if (glen > 0 && 1 + glen <= static_cast<int>(guidRemaining)) {
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
    return true;
}

namespace {

// Build a PlayerJoined reliable payload describing peer `slot` (its eid +
// nick). Wire layout (parsed field-by-field, same as Join):
//   [uint8 slot][uint32 eid][uint8 nicklen][nick UTF-8]
std::vector<uint8_t> BuildPlayerJoinedPayload(uint8_t slot, uint32_t eid,
                                              const std::wstring& nick) {
    std::vector<uint8_t> out;
    out.resize(5);
    out[0] = slot;
    std::memcpy(out.data() + 1, &eid, 4);
    std::vector<uint8_t> nickUtf8 = ToUtf8(nick);
    if (nickUtf8.size() > 200) nickUtf8.resize(200);
    out.push_back(static_cast<uint8_t>(nickUtf8.size()));
    out.insert(out.end(), nickUtf8.begin(), nickUtf8.end());
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
                                     joinerEid, joinerNick);
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
                                     NicknameForSlot(x));
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
    if (nickRemaining > 0) {
        const int len = nickStart[0];
        if (1 + len <= static_cast<int>(nickRemaining) && len > 0)
            nick = FromUtf8(nickStart + 1, len);
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

void AnnouncePeerSpawned(net::Role role, int slot) {
    // Called from net_pump the moment a remote peer's PUPPET spawns. net_pump now calls this ONLY for
    // the CLIENT role: slot 0 is the HOST whose game WE joined ("Joined <host>'s game"); a higher slot
    // is a cross-peer client whose puppet just spawned here ("<nick> joined the game"). Both are gated
    // on the client's own g_worldReadyAnnounced (net_pump's puppet-spawn block), so they fire at this
    // peer's world-ready. The HOST announces a joiner via OnClientWorldReady (below), NOT here -- the
    // first pose can be a pre-world pose (user 2026-06-17). The role arg is kept for the slot-0 phrasing.
    const std::wstring nick = NicknameForSlot(static_cast<uint8_t>(slot));
    // 5 s delay: the world-ready announce fires before the loading screen visually clears, so showing the
    // join line immediately looks premature (user 2026-06-21). Let the join settle first.
    if (role == net::Role::Client && slot == 0) {
        coop::chat_feed::PushDelayed(L"Joined " + nick + L"'s game", 5000);
    } else {
        coop::chat_feed::PushDelayed(nick + L" joined the game", 5000);
    }
}

void OnClientWorldReady(int slot) {
    // HOST-side "<nick> joined the game" -- the authoritative in-world announce (see the header). Fired
    // from event_feed when a client's ClientWorldReady reliable lands; latched once per join so a
    // re-sent ClientWorldReady doesn't repeat the line.
    UE_ASSERT_GAME_THREAD("g_joinAnnouncedBySlot (OnClientWorldReady)");
    if (slot < 1 || slot >= net::kMaxPeers) return;  // slot 0 = host self; never "joins"
    if (g_joinAnnouncedBySlot[slot]) return;          // already announced for this join
    g_joinAnnouncedBySlot[slot] = true;
    coop::chat_feed::PushDelayed(NicknameForSlot(slot) + L" joined the game", 5000);  // 5 s: let the joiner finish loading
    UE_LOGI("player_handshake: slot %d joined the game (announced on ClientWorldReady, +5s)", slot);
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

}  // namespace coop::player_handshake
