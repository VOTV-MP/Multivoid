#include "coop/event_feed.h"

#include "coop/element/registry.h"

#include "coop/item_activate.h"
#include "coop/net/session.h"
#include "coop/npc_sync.h"
#include "coop/players_registry.h"
#include "coop/remote_player.h"
#include "coop/remote_prop.h"
#include "coop/weather_sync.h"
#include "coop/dev/restore_vitals.h"
#include "coop/dev/teleport_client.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/hud_feed.h"
#include "ue_wrap/log.h"
#include "ue_wrap/sdk_profile.h"

#include <windows.h>

#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace coop::event_feed {

namespace {

std::wstring g_localNick = L"Player";
// Per-slot nickname + per-slot connection edge. Single-peer scalars
// would let two clients' Joins overwrite each other and the "X left"
// message would fire on the aggregate-disconnect with whoever's nick
// happened to be cached at the time (NOT the actually-departing peer).
// Placeholder ("Remote player") is shown ONLY in the rare case where
// the peer drops before its Join reliable message lands.
std::array<std::wstring, net::kMaxPeers> g_remoteNickBySlot{
    L"Remote player", L"Remote player", L"Remote player", L"Remote player"
};
std::array<bool, net::kMaxPeers> g_lastConnectedBySlot{};
// Per-slot Join tracking: a single global Join-sent bit fan-out gives
// late-joining peers no Join packet (their event_feed sees nothing from
// us + sticks with the "Remote player" placeholder forever). Per-slot
// + reset-on-disconnect means a reconnect re-announces.
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

void OnSessionStart() {
    // File-scope per-slot state persists across Session::Stop()/Start() in
    // the same process. The harness only resets its own `g_wasConnected`
    // bit on each Start; we own the corresponding event_feed state and
    // reset it here so a restart of the session sees clean per-slot
    // edge-detector input.
    for (auto& nick : g_remoteNickBySlot) nick = L"Remote player";
    g_lastConnectedBySlot.fill(false);
    g_joinSentBySlot.fill(false);
}

void Update(net::Session& session, void* localPlayer) {
    // Fan the latest RTT across every live puppet so each nameplate
    // shows "<nick> (<ping>ms)". Session today exposes only an
    // aggregate lastRttMs() (first-connected-peer's RTT); per-slot
    // RTT can land later. Fanning beats updating only slot 1.
    const int rtt = session.lastRttMs();
    for (int slot = 0; slot < net::kMaxPeers; ++slot) {
        RemotePlayer* p = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
        if (p) p->SetPing(rtt);
    }

    // Per-slot Join announcement. For each slot that we haven't yet sent
    // our Join to AND is currently connected, send Join via
    // SendReliableToSlot. The Join payload is built lazily on first
    // need: in steady state (all peers have received our Join) the
    // joinPayload vector is never constructed, so we don't pay the
    // ToUtf8 + WideCharToMultiByte + vector heap-alloc cost every
    // 8ms at 125Hz NetPumpTick.
    std::vector<uint8_t> joinPayload;
    bool joinPayloadBuilt = false;
    for (int slot = 0; slot < net::kMaxPeers; ++slot) {
        const bool slotConnected = session.IsSlotConnected(slot);
        // Per-slot disconnect edge: when a specific slot transitions
        // connected -> disconnected, post the departing peer's nick.
        if (g_lastConnectedBySlot[slot] && !slotConnected) {
            ue_wrap::hud_feed::Push(g_remoteNickBySlot[slot] + L" left the game");
            // Reset Join-sent so the next reconnect re-announces.
            g_joinSentBySlot[slot] = false;
        }
        // Per-slot connect edge: send our Join to this slot.
        if (slotConnected && !g_joinSentBySlot[slot]) {
            if (!joinPayloadBuilt) {
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
            // If send fails (transient), we'll retry next tick.
        }
        g_lastConnectedBySlot[slot] = slotConnected;
    }

    // Drain delivered reliable messages.
    net::Session::ReliableMessage msg;
    while (session.TryGetReliable(msg)) {
        switch (msg.kind) {
        case net::ReliableKind::Join: {
            // Per-slot nickname keyed on the sender's peer slot. A single
            // global scalar would let two clients on host clobber each
            // other's nick on every Join.
            const int senderSlot = msg.senderPeerSlot;
            if (senderSlot < 0 || senderSlot >= net::kMaxPeers) {
                UE_LOGW("event_feed: Join has invalid senderPeerSlot=%d -- dropping",
                        senderSlot);
                break;
            }
            std::wstring nick = g_remoteNickBySlot[senderSlot];
            if (msg.payloadLen > 0) {
                const int len = msg.payload[0];
                if (1 + len <= static_cast<int>(msg.payloadLen) && len > 0)
                    nick = FromUtf8(msg.payload + 1, len);
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
            break;
        }
        case net::ReliableKind::PropRelease: {
            // v5: peer released a held prop. Dispatch to remote_prop which
            // re-enables SimulatePhysics + sets linear/angular velocity, and
            // fires Aprop_C.thrown if the launch crosses the throw threshold.
            if (msg.payloadLen < sizeof(net::PropReleasePayload)) {
                UE_LOGW("event_feed: PropRelease payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::PropReleasePayload));
                break;
            }
            net::PropReleasePayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            // Trust-boundary validation: a NaN/Inf or absurd-magnitude velocity
            // reaches UPrimitiveComponent::SetPhysicsLinearVelocity ->
            // SetPhysicsAngularVelocityInDegrees -> PhysX UB. Reject before
            // dispatch.
            const float vals[6] = {p.linVelX, p.linVelY, p.linVelZ,
                                   p.angVelX, p.angVelY, p.angVelZ};
            bool finite = true;
            for (float v : vals) {
                if (!std::isfinite(v)) { finite = false; break; }
            }
            if (!finite) {
                UE_LOGW("event_feed: PropRelease velocity non-finite -- dropping");
                break;
            }
            // Linear velocity bound: realistic throws peak at a few thousand
            // cm/s. 1e6 cm/s = 10 km/s -- well beyond any legitimate throw and
            // below any value that would teleport a body to infinity in one
            // tick. Angular velocity bound: a fast tumble is ~3600 deg/s
            // (10 rps); 1e6 is generous headroom.
            constexpr float kMaxLinVel = 1.0e6f;
            constexpr float kMaxAngVel = 1.0e6f;
            if (std::fabs(p.linVelX) > kMaxLinVel ||
                std::fabs(p.linVelY) > kMaxLinVel ||
                std::fabs(p.linVelZ) > kMaxLinVel ||
                std::fabs(p.angVelX) > kMaxAngVel ||
                std::fabs(p.angVelY) > kMaxAngVel ||
                std::fabs(p.angVelZ) > kMaxAngVel) {
                UE_LOGW("event_feed: PropRelease velocity out of bounds (lin=(%.1f,%.1f,%.1f) ang=(%.1f,%.1f,%.1f)) -- dropping",
                        p.linVelX, p.linVelY, p.linVelZ,
                        p.angVelX, p.angVelY, p.angVelZ);
                break;
            }
            remote_prop::OnRelease(msg.senderPeerSlot, p, localPlayer);
            break;
        }
        case net::ReliableKind::PropSpawn: {
            // v5 Bug C: peer dropped an inventory item -- spawn a matching
            // Aprop_X_C locally so subsequent PropPose updates resolve.
            if (msg.payloadLen < sizeof(net::PropSpawnPayload)) {
                UE_LOGW("event_feed: PropSpawn payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::PropSpawnPayload));
                break;
            }
            net::PropSpawnPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            // Trust-boundary validation: any of the 18 floats (loc/rot/scale +
            // 2 vel vectors) NaN/Inf or out-of-bound -> SpawnActor +
            // SetPhysics* could crash PhysX. Reject before dispatch.
            const float vals[18] = {
                p.locX, p.locY, p.locZ,
                p.rotPitch, p.rotYaw, p.rotRoll,
                p.scaleX, p.scaleY, p.scaleZ,
                p.initLinVelX, p.initLinVelY, p.initLinVelZ,
                p.initAngVelX, p.initAngVelY, p.initAngVelZ,
                0.f, 0.f, 0.f  // pad
            };
            bool finite = true;
            for (int i = 0; i < 15; ++i) {  // skip the 3 pad slots
                if (!std::isfinite(vals[i])) { finite = false; break; }
            }
            if (!finite) {
                UE_LOGW("event_feed: PropSpawn floats non-finite -- dropping");
                break;
            }
            constexpr float kMaxCoord = 1.0e6f;
            constexpr float kMaxVel   = 1.0e6f;
            if (std::fabs(p.locX) > kMaxCoord || std::fabs(p.locY) > kMaxCoord ||
                std::fabs(p.locZ) > kMaxCoord) {
                UE_LOGW("event_feed: PropSpawn location out of bounds (%.1f, %.1f, %.1f)",
                        p.locX, p.locY, p.locZ);
                break;
            }
            if (std::fabs(p.initLinVelX) > kMaxVel || std::fabs(p.initLinVelY) > kMaxVel ||
                std::fabs(p.initLinVelZ) > kMaxVel ||
                std::fabs(p.initAngVelX) > kMaxVel || std::fabs(p.initAngVelY) > kMaxVel ||
                std::fabs(p.initAngVelZ) > kMaxVel) {
                UE_LOGW("event_feed: PropSpawn velocity out of bounds");
                break;
            }
            // Clamp class/key lengths defensively (they're uint8 but the
            // sender could lie). 63/31 are the struct caps.
            if (p.className.len > 63) {
                UE_LOGW("event_feed: PropSpawn className.len=%u > 63 -- dropping", p.className.len);
                break;
            }
            if (p.key.len > 31) {
                UE_LOGW("event_feed: PropSpawn key.len=%u > 31 -- dropping", p.key.len);
                break;
            }
            // Phase 5N Stream B (2026-05-24): drop wire-spawns of
            // intermediate-variant classes that the receiver doesn't want
            // (mushroom7_C growing state). Host-authoritative growth
            // pipeline -- the mature variant (mushroom_C) will arrive when
            // host's transform-timer fires. Mirrors the role==Client +
            // IsClientSuppressedPropClass check in harness.cpp::
            // GrabObserver_Aprop_Init_POST so the suppression is symmetric:
            // never spawn locally AND never accept wire spawns of these.
            {
                std::wstring cls;
                cls.reserve(p.className.len);
                for (uint8_t i = 0; i < p.className.len; ++i) {
                    cls.push_back(static_cast<wchar_t>(static_cast<unsigned char>(p.className.data[i])));
                }
                if (cls == ue_wrap::profile::name::PropMushroomGrowingClass) {
                    UE_LOGI("event_feed: PropSpawn drop -- intermediate-variant class '%ls' suppressed on this peer (host-authoritative; mature variant will arrive when host transforms)",
                            cls.c_str());
                    break;
                }
            }
            remote_prop::OnSpawn(p);
            break;
        }
        case net::ReliableKind::PropDestroy: {
            // v5 Inc2: peer destroyed a prop -- delete the matching local
            // actor (resolved via FindByKeyString). Receiver-side
            // K2_DestroyActor is echo-suppressed via the incoming-destroy
            // set so it doesn't bounce back to the sender.
            //
            // TRUST BOUNDARY (audit fix #4, 2026-05-25): with bidirectional
            // destroy, CLIENT can command HOST to destroy any prop by
            // wire-Key. Acceptable for LAN coop (trusted peers); review
            // before Internet coop -- a malicious client could replay
            // crafted Keys to destroy host's quest items. Mitigation if
            // needed: authority model (host validates destroy requests
            // against current world state / quest progress).
            if (msg.payloadLen < sizeof(net::PropDestroyPayload)) {
                UE_LOGW("event_feed: PropDestroy payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::PropDestroyPayload));
                break;
            }
            net::PropDestroyPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            if (p.key.len > 31) {
                UE_LOGW("event_feed: PropDestroy key.len=%u > 31 -- dropping", p.key.len);
                break;
            }
            // 2026-05-25 cross-peer destroy: pass localPlayer so OnDestroy
            // can release a held PHC grab (mainPlayer.grabbing_actor ==
            // doomed) before K2_DestroyActor. Prevents UPhysicsHandle
            // Component::TickComponent reading a dangling GrabbedComponent
            // ptr next frame.
            remote_prop::OnDestroy(p, localPlayer);
            break;
        }
        case net::ReliableKind::EntitySpawn: {
            // Inc3 (2026-05-28 NEXT): host-broadcast NPC spawn. Validate
            // size + className.len at the trust boundary here; npc_sync::
            // OnEntitySpawn does the full per-field validation (finite +
            // bounds + allowlist + dedup). UFunction calls inside
            // OnEntitySpawn are game-thread only, so dispatch via GT::Post.
            if (msg.payloadLen < sizeof(net::EntitySpawnPayload)) {
                UE_LOGW("event_feed: EntitySpawn payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::EntitySpawnPayload));
                break;
            }
            net::EntitySpawnPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            if (p.className.len > 63) {
                UE_LOGW("event_feed: EntitySpawn className.len=%u > 63 -- dropping",
                        p.className.len);
                break;
            }
            net::EntitySpawnPayload pCopy = p;
            ue_wrap::game_thread::Post([pCopy] {
                ::coop::npc_sync::OnEntitySpawn(pCopy);
            });
            break;
        }
        case net::ReliableKind::EntityDestroy: {
            // Inc3: host-broadcast NPC destroy. Dispatch to npc_sync::
            // OnEntityDestroy (game-thread UFunction call).
            if (msg.payloadLen < sizeof(net::EntityDestroyPayload)) {
                UE_LOGW("event_feed: EntityDestroy payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::EntityDestroyPayload));
                break;
            }
            net::EntityDestroyPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            net::EntityDestroyPayload pCopy = p;
            ue_wrap::game_thread::Post([pCopy] {
                ::coop::npc_sync::OnEntityDestroy(pCopy);
            });
            break;
        }
        case net::ReliableKind::RestoreVitals: {
            // 2026-05-25 LATE +5h (F3 dev key): peer pressed F3 to refill
            // food/sleep/health/coffeePower. No payload to validate -- the
            // action is fixed. Idempotent so an echo bounce is harmless.
            ue_wrap::game_thread::Post([] { ::coop::dev::restore_vitals::ApplyLocally(); });
            break;
        }
        case net::ReliableKind::TeleportClient: {
            // 2026-05-25 LATE +5h (F4 dev key): host snapshotted its pose and
            // sent it; client applies to local mainPlayer. Host echo is
            // a no-op below.
            if (msg.payloadLen < sizeof(net::TeleportClientPayload)) {
                UE_LOGW("event_feed: TeleportClient payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::TeleportClientPayload));
                break;
            }
            net::TeleportClientPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            // Trust-boundary validation (same defensive pattern as
            // PropRelease velocity check at line 196 above): reject NaN/Inf
            // before the engine call. UE's K2_TeleportTo with a NaN
            // location asserts inside FSweepData::ClampSweepParameters.
            const float vals[6] = {p.locX, p.locY, p.locZ, p.rotPitch, p.rotYaw, p.rotRoll};
            bool finite = true;
            for (float v : vals) { if (!std::isfinite(v)) { finite = false; break; } }
            if (!finite) {
                UE_LOGW("event_feed: TeleportClient payload non-finite -- dropping");
                break;
            }
            // AABB bound (audit-fix 2026-05-25 LATE +5h): finite-check alone
            // allows extreme-but-finite coords (e.g. 1e30) that would still
            // assert inside the engine's teleport math. Mirror the project's
            // own kMaxCoord = 1.0e6f trust boundary from ValidatePose /
            // PropSpawnPayload receiver -- one consistent magnitude rule for
            // any world-position payload. Rotations don't need a magnitude
            // bound because FRotator components are angles (any value is
            // normalized inside K2_TeleportTo).
            if (std::fabs(p.locX) > net::kMaxCoord ||
                std::fabs(p.locY) > net::kMaxCoord ||
                std::fabs(p.locZ) > net::kMaxCoord) {
                UE_LOGW("event_feed: TeleportClient location out of bounds (%.1f,%.1f,%.1f) -- dropping",
                        p.locX, p.locY, p.locZ);
                break;
            }
            // Host echo gate: if WE are the host, this packet originated from
            // us (broadcast bounced back via the reliable channel). Applying
            // would teleport host to its own pose -- harmless but pointless.
            // Skip explicitly so we don't accidentally collide with whatever
            // host was doing the moment it pressed F4.
            if (session.role() == net::Role::Host) {
                UE_LOGI("event_feed: TeleportClient self-echo on host -- no-op");
                break;
            }
            ::coop::dev::teleport_client::ApplyArgs args{
                p.locX, p.locY, p.locZ,
                p.rotPitch, p.rotYaw, p.rotRoll,
            };
            ue_wrap::game_thread::Post([args] { ::coop::dev::teleport_client::ApplyLocally(args); });
            break;
        }
        case net::ReliableKind::ItemActivate: {
            // Phase 5F flashlight (and future radio/torch/lamp) -- peer's
            // item state changed and produces a WORLD effect both peers
            // must see. For Case (b) flashlight: apply to the puppet's
            // light_R. RE doc: research/findings/votv-flashlight-RE-
            // 2026-05-25.md.
            if (msg.payloadLen < sizeof(net::ItemActivatePayload)) {
                UE_LOGW("event_feed: ItemActivate payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::ItemActivatePayload));
                break;
            }
            net::ItemActivatePayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            // Trust-boundary: state is a uint8 but only 0/1 are valid.
            if (p.state != 0 && p.state != 1) {
                UE_LOGW("event_feed: ItemActivate state=%u out of range -- dropping",
                        static_cast<unsigned>(p.state));
                break;
            }
            // Self-echo guard: if peerSessionId == our LocalPeerId,
            // the packet is a loopback bounce. A hardcoded 1v1 selfId
            // (host=0, client=1) would let two clients with peer-ids
            // 1 and 2 both self-echo-drop each other's ItemActivate.
            // Read from the central Registry instead.
            const uint8_t selfId = coop::players::Registry::Get().LocalPeerId();
            if (p.peerSessionId == selfId) {
                UE_LOGI("event_feed: ItemActivate self-echo (peerSessionId=%u) -- dropping",
                        static_cast<unsigned>(p.peerSessionId));
                break;
            }
            // 1v1: the puppet is the only remote. May be null if this
            // packet beat the first PoseSnapshot (the puppet is spawned
            // lazily on first pose; ItemActivate rides the reliable
            // channel and CAN arrive first under a connect-edge burst).
            // Inc5: hand to ApplyToPuppetOrDefer which stashes the
            // payload if the puppet isn't ready; TickConnect drains it
            // once the puppet appears in the registry.
            //
            // Audit C1 (2026-05-27): capture peerId only; re-fetch puppet
            // INSIDE the lambda. Capturing the raw void* here risks UAF
            // because Destroy() can run on the game thread between this
            // post and the lambda dispatch, recycling the GUObjectArray
            // slot.
            net::ItemActivatePayload pCopy = p;
            const uint8_t peerId = p.peerSessionId;
            ue_wrap::game_thread::Post([peerId, pCopy] {
                ::coop::RemotePlayer* rp =
                    ::coop::players::Registry::Get().Puppet(peerId);
                void* puppetNow = (rp && rp->valid()) ? rp->GetActor() : nullptr;
                ::coop::item_activate::ApplyToPuppetOrDefer(peerId, puppetNow, pCopy);
            });
            break;
        }
        case net::ReliableKind::RedSky: {
            // Phase 5W Inc-fix-2 (2026-05-27): one-shot/toggle red-sky
            // story-event sync. Host's POST observer on spawnRedSky +
            // redSky.set caught the change; broadcast it. Receiver
            // invokes the same chain on its local gamemode.
            if (msg.payloadLen < sizeof(net::RedSkyPayload)) {
                UE_LOGW("event_feed: RedSky payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::RedSkyPayload));
                break;
            }
            net::RedSkyPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            if (session.role() == net::Role::Host) {
                UE_LOGI("event_feed: RedSky received on host -- dropping");
                break;
            }
            if (p.state != 0 && p.state != 1) {
                UE_LOGW("event_feed: RedSky state=%u out of range -- dropping",
                        static_cast<unsigned>(p.state));
                break;
            }
            net::RedSkyPayload pCopy = p;
            ue_wrap::game_thread::Post([pCopy] {
                ::coop::weather_sync::ApplyRedSky(pCopy);
            });
            break;
        }
        case net::ReliableKind::LightningStrike: {
            // Phase 5W Inc2 (2026-05-27): discrete strike event. Host's
            // POST observer on BeginDeferredActorSpawnFromClass caught
            // an AlightningStrike_C spawn (BP-internal SpawnActor inside
            // AdaynightCycle_C::timerLightning) and broadcast the
            // strike's world location. Client suppressed its own
            // timerLightning via Inc1's interceptor so no local strike
            // happened; this packet drives the visual.
            if (msg.payloadLen < sizeof(net::LightningStrikePayload)) {
                UE_LOGW("event_feed: LightningStrike payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::LightningStrikePayload));
                break;
            }
            net::LightningStrikePayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            if (session.role() == net::Role::Host) {
                UE_LOGI("event_feed: LightningStrike received on host -- dropping");
                break;
            }
            // Trust boundary: validate loc finite + within sane bounds.
            if (!std::isfinite(p.locX) || !std::isfinite(p.locY) || !std::isfinite(p.locZ) ||
                std::fabs(p.locX) > coop::net::kMaxCoord ||
                std::fabs(p.locY) > coop::net::kMaxCoord ||
                std::fabs(p.locZ) > coop::net::kMaxCoord) {
                UE_LOGW("event_feed: LightningStrike loc out of bounds (%.0f, %.0f, %.0f) -- dropping",
                        p.locX, p.locY, p.locZ);
                break;
            }
            net::LightningStrikePayload pCopy = p;
            ue_wrap::game_thread::Post([pCopy] {
                ::coop::weather_sync::ApplyLightningStrike(pCopy);
            });
            break;
        }
        case net::ReliableKind::WeatherState: {
            // Phase 5W Inc1 (2026-05-26): host-authoritative weather state.
            // Sender = host. Receiver looks up local AdaynightCycle_C and
            // invokes the cycle's mutator UFunctions to apply each delta.
            // See coop/weather_sync.cpp::ApplyFromHost for the full apply
            // logic + research/findings/votv-weather-DESIGN-2026-05-26.md.
            if (msg.payloadLen < sizeof(net::WeatherStatePayload)) {
                UE_LOGW("event_feed: WeatherState payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::WeatherStatePayload));
                break;
            }
            net::WeatherStatePayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            // Self-echo guard: weather is host->client only; if our role
            // says we ARE the host, a WeatherState packet must be a loopback
            // bounce (we'd never send to ourselves but defensive). Drop.
            if (session.role() == net::Role::Host) {
                UE_LOGI("event_feed: WeatherState received on host -- dropping "
                        "(host is the authority; no inbound from client)");
                break;
            }
            // Trust-boundary: validate floats finite + within sane range.
            // Rain scalars are unitless in [0, ~10] in BP usage; allow a
            // generous (-1e3, 1e3) range to catch garbage without
            // legitimately clamping anything.
            const float vals[4] = {
                p.rainStrength, p.rainLightningChance,
                p.rainDeactivateChance, p.rainWindSpeed
            };
            bool bad = false;
            for (float v : vals) {
                if (!std::isfinite(v) || std::fabs(v) > 1.0e3f) { bad = true; break; }
            }
            if (bad) {
                UE_LOGW("event_feed: WeatherState scalars out of bounds (rain=%.2f "
                        "lc=%.2f dc=%.2f ws=%.2f) -- dropping",
                        p.rainStrength, p.rainLightningChance,
                        p.rainDeactivateChance, p.rainWindSpeed);
                break;
            }
            net::WeatherStatePayload pCopy = p;
            ue_wrap::game_thread::Post([pCopy] {
                ::coop::weather_sync::ApplyFromHost(pCopy);
            });
            break;
        }
        case net::ReliableKind::AssignPeerSlot: {
            // Host tells us which peer slot we were assigned. Without
            // this the client would self-stamp LocalPeerId=1 from a
            // hardcoded 1v1 mapping, and a second client with the same
            // local ID would silently self-echo-drop the first's
            // ItemActivate as a "loopback bounce" (see Update Join/echo
            // guard above).
            if (msg.payloadLen < sizeof(net::AssignPeerSlotPayload)) {
                UE_LOGW("event_feed: AssignPeerSlot payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::AssignPeerSlotPayload));
                break;
            }
            net::AssignPeerSlotPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            // Trust boundary: only the host sends this; reject on host.
            if (session.role() == net::Role::Host) {
                UE_LOGW("event_feed: AssignPeerSlot received on host -- dropping "
                        "(host self-assigns slot 0; no inbound from client)");
                break;
            }
            // Slot must be a valid CLIENT slot (1..kMaxPeers-1). Slot 0 is
            // the host's reserved local-self slot.
            if (p.slot < 1 || p.slot >= net::kMaxPeers) {
                UE_LOGW("event_feed: AssignPeerSlot slot=%u out of range [1..%u) -- dropping",
                        p.slot, static_cast<unsigned>(net::kMaxPeers));
                break;
            }
            coop::players::Registry::Get().SetLocalPeerId(p.slot);
            UE_LOGI("event_feed: host assigned us peer slot %u (Registry::LocalPeerId now %u)",
                    p.slot, coop::players::Registry::Get().LocalPeerId());
            break;
        }
        default: {
            // Audit-fix 2026-05-25 LATE +5h: log-and-drop unknown ReliableKind
            // values instead of silently discarding. A peer running a newer
            // protocol could send a kind we don't yet handle; this surfaces
            // the gap in the log rather than letting it look like nothing
            // happened. Existing pattern across the project.
            UE_LOGW("event_feed: unknown ReliableKind %u -- dropping",
                    static_cast<unsigned>(msg.kind));
            break;
        }
        }
    }
}

}  // namespace coop::event_feed
