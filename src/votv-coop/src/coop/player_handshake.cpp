// coop/player_handshake.cpp -- see coop/player_handshake.h.

#include "coop/player_handshake.h"

#include "coop/element/player.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/players_registry.h"
#include "coop/remote_player.h"
#include "ue_wrap/hud_feed.h"
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
//     hud_feed widget text and could in theory escape out of UMG bindings.
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

void Reset() {
    for (auto& nick : g_remoteNickBySlot) nick = L"Remote player";
    g_joinSentBySlot.fill(false);
}

void MaybeSendJoinToSlot(net::Session& session, int slot,
                         std::vector<uint8_t>& joinPayload,
                         bool& joinPayloadBuilt) {
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
    if (slot < 0 || slot >= net::kMaxPeers) return;
    g_joinSentBySlot[slot] = false;
    // N-5 (2026-05-29 audit): clear the stashed nickname so a subsequent peer
    // reusing the same slot before its Join arrives doesn't display the
    // departed peer's name in the HUD toasts. NicknameForSlot falls back to
    // a placeholder when this is empty.
    g_remoteNickBySlot[slot].clear();
}

const std::wstring& NicknameForSlot(int slot) {
    static const std::wstring kPlaceholder = L"Remote player";
    if (slot < 0 || slot >= net::kMaxPeers) return kPlaceholder;
    return g_remoteNickBySlot[slot];
}

bool HandleJoinMessage(net::Session& session,
                       const net::Session::ReliableMessage& msg) {
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
    if (nickRemaining > 0) {
        const int len = nickStart[0];
        if (1 + len <= static_cast<int>(nickRemaining) && len > 0)
            nick = FromUtf8(nickStart + 1, len);
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
    // our floating UMG nameplate + the hud_feed widget. Without
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
    // Role-aware phrasing (2026-05-27, user feedback): on the CLIENT
    // the Join packet arrives FROM the host -- saying "<host> joined
    // the game" reads backwards (the client is the one who joined).
    // Phrase from the receiver's POV.
    if (session.role() == net::Role::Client) {
        ue_wrap::hud_feed::Push(L"Successfully joined " + nick + L"'s game");
    } else {
        ue_wrap::hud_feed::Push(nick + L" joined the game");
    }
    return true;
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
