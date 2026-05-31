#include "coop/event_feed.h"

#include "coop/element/player.h"
#include "coop/element/registry.h"

#include "coop/item_activate.h"
#include "coop/net/session.h"
#include "coop/player_damage.h"
#include "coop/npc_mirror.h"
#include "coop/player_handshake.h"
#include "coop/players_registry.h"
#include "coop/remote_player.h"
#include "coop/remote_prop.h"
#include "coop/remote_prop_spawn.h"
#include "coop/weather_sync.h"
#include "coop/dev/restore_vitals.h"
#include "coop/dev/teleport_client.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/hud_feed.h"
#include "ue_wrap/log.h"
#include "ue_wrap/sdk_profile.h"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace coop::event_feed {

namespace {

// Per-slot connect/disconnect edge detector. The Join-sent latch and
// per-slot nicknames live in coop::player_handshake (C5 extraction
// 2026-05-29); event_feed only needs the prior-connected bit to fire
// the "<X> left the game" hud message on the disconnect transition.
std::array<bool, net::kMaxPeers> g_lastConnectedBySlot{};

// PR-FOUNDATION-1 (2026-05-29): role-range validation for an inbound
// eid-carrying packet. Without this, a malicious client peer can stamp
// ItemActivate/RedSky/Lightning/Weather with the HOST's senderElementId;
// Registry::Get resolves to the host's Player Element and the receiver
// applies the packet's effect under host identity. The range partition
// makes that impersonation detectable at the wire boundary: host-role
// packets MUST carry host-range eids; client-role packets MUST carry
// peer-range eids. Returns true when the eid is in-range OR when no
// compare is possible (0 sentinel / invalid sender slot). Logs +
// returns false on out-of-range.
//
// v14 / v15 also performed an 8-bit syncContext compare here (the
// VerifySenderContext function). v16 PR-FOUNDATION-1b retired that
// layer entirely: per-peer stale-generation defense now lives in
// Session::HandleMessage's senderEpoch latch, applied uniformly to
// EVERY inbound packet by the transport layer (not per-payload by the
// receiver dispatch). This helper kept the role-range half because
// it's a wire-format trust boundary (the eid range is the sender's
// claimed role), independent of the stale-gen defense.
bool VerifySenderEidRange(int senderPeerSlot,
                          uint32_t senderElementId,
                          const char* kind) {
    if (senderElementId == 0u ||
        senderElementId == coop::element::kInvalidId) {
        return true;  // 0 sentinel; no compare. peer-slot fallback applies.
    }
    if (senderPeerSlot < 0) return true;  // unknown sender; skip range check
    const bool senderIsHost = (senderPeerSlot == 0);
    if (!coop::element::Registry::IsAllowedSenderEid(
            senderIsHost, senderElementId)) {
        UE_LOGW("event_feed: %s senderElementId=0x%08x out of allowed %s "
                "range (senderPeerSlot=%d) -- dropping (role impersonation?)",
                kind, senderElementId,
                senderIsHost ? "host" : "peer",
                senderPeerSlot);
        return false;
    }
    return true;
}

}  // namespace

void SetLocalNickname(const std::wstring& nick) {
    coop::player_handshake::SetLocalNickname(nick);
}

void OnSessionStart() {
    // File-scope per-slot state persists across Session::Stop()/Start() in
    // the same process. The harness only resets its own `g_wasConnected`
    // bit on each Start; we own the corresponding event_feed state and
    // reset it here so a restart of the session sees clean per-slot
    // edge-detector input.
    g_lastConnectedBySlot.fill(false);
    coop::player_handshake::Reset();
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

    // Per-slot Join announcement + per-slot "left the game" hud. The
    // Join payload is built lazily on first need: in steady state
    // (all peers have received our Join) the joinPayload vector is
    // never constructed, so we don't pay the ToUtf8 + heap-alloc cost
    // every 8 ms at 125 Hz NetPumpTick.
    std::vector<uint8_t> joinPayload;
    bool joinPayloadBuilt = false;
    for (int slot = 0; slot < net::kMaxPeers; ++slot) {
        // Two distinct edges:
        // - LEFT toast / disconnect cleanup: IsSlotConnected (cleared in
        //   the Closed callback) is the right signal.
        // - Send Join: must wait for IsSlotReady (lanes configured in the
        //   Connected callback), otherwise SendReliableToSlot's m_idxLane
        //   rides GNS lane 0 instead of the assigned Normal lane,
        //   undermining PR-3's head-of-line isolation for the first
        //   reliable message per peer. N-4 (2026-05-29 audit).
        const bool slotConnected = session.IsSlotConnected(slot);
        const bool slotReady = session.IsSlotReady(slot);
        if (g_lastConnectedBySlot[slot] && !slotConnected) {
            ue_wrap::hud_feed::Push(
                coop::player_handshake::NicknameForSlot(slot) + L" left the game");
            coop::player_handshake::OnSlotDisconnected(slot);
        }
        if (slotReady) {
            coop::player_handshake::MaybeSendJoinToSlot(
                session, slot, joinPayload, joinPayloadBuilt);
        }
        g_lastConnectedBySlot[slot] = slotConnected;
    }

    // Drain delivered reliable messages.
    net::Session::ReliableMessage msg;
    while (session.TryGetReliable(msg)) {
        switch (msg.kind) {
        case net::ReliableKind::Join: {
            coop::player_handshake::HandleJoinMessage(session, msg);
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
            // senderPeerSlot threads into remote_prop::OnRelease for routing
            // into g_drives[slot]. Range-check before use so a malformed -1
            // or out-of-range value can't OOB the slot array.
            if (msg.senderPeerSlot < 0 || msg.senderPeerSlot >= net::kMaxPeers) {
                UE_LOGW("event_feed: PropRelease invalid senderPeerSlot=%d -- dropping",
                        msg.senderPeerSlot);
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
            // PR-FOUNDATION-1 (2026-05-29): elementId range trust. The
            // sender's role determines which allocation range its
            // PropSpawn.elementId is allowed to land in (host -> host
            // range; client -> peer range per the A2 v12 contract:
            // chipPile/clump/trashBits broadcast client->host so a
            // client-sourced PropSpawn carries a peer-range eid). A
            // packet carrying an eid outside its sender's permitted
            // range is forged or relay-loop bugged and must be dropped
            // at the boundary before it reaches RegisterPropMirror and
            // collides with the receiver's own allocator. Closes
            // D2-2 / E-1's PropSpawn gap. (The pre-existing inline
            // host-range check in npc_mirror :: OnEntitySpawn becomes
            // a call to the same helper below.)
            if (msg.senderPeerSlot >= 0) {
                const bool senderIsHost = (msg.senderPeerSlot == 0);
                if (!coop::element::Registry::IsAllowedSenderEid(senderIsHost, p.elementId)) {
                    UE_LOGW("event_feed: PropSpawn elementId=0x%08x out of allowed "
                            "%s range (senderPeerSlot=%d) -- dropping",
                            p.elementId,
                            senderIsHost ? "host" : "peer",
                            msg.senderPeerSlot);
                    break;
                }
            }
            // (v15 added a senderContext compare here for stale-generation
            // defense; v16 PR-FOUNDATION-1b moved that to the packet
            // header's senderEpoch latched in Session::HandleMessage.)
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
            remote_prop_spawn::OnSpawn(p, msg.senderPeerSlot);
            break;
        }
        case net::ReliableKind::PropDestroy: {
            // v5 Inc2: peer destroyed a prop -- delete the matching local
            // actor (resolved via FindByKeyString). Receiver-side
            // K2_DestroyActor is echo-suppressed via the incoming-destroy
            // set so it doesn't bounce back to the sender.
            //
            // TRUST BOUNDARY: with bidirectional destroy, CLIENT can command
            // HOST to destroy any prop by wire-Key. Acceptable for LAN coop
            // (trusted peers); review before Internet coop -- a malicious
            // client could replay crafted Keys to destroy host's quest items.
            // Mitigation if needed: authority model (host validates destroy
            // requests against current world state / quest progress).
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
            // PR-FOUNDATION-1 (2026-05-29): elementId range trust. Same
            // rule as PropSpawn -- the sender's role determines the
            // permitted allocation range. PropDestroy with a forged
            // eid would otherwise reach UnregisterPropMirror with a
            // host-range eid the sender never legitimately allocated.
            if (msg.senderPeerSlot >= 0) {
                const bool senderIsHost = (msg.senderPeerSlot == 0);
                if (!coop::element::Registry::IsAllowedSenderEid(senderIsHost, p.elementId)) {
                    UE_LOGW("event_feed: PropDestroy elementId=0x%08x out of allowed "
                            "%s range (senderPeerSlot=%d) -- dropping",
                            p.elementId,
                            senderIsHost ? "host" : "peer",
                            msg.senderPeerSlot);
                    break;
                }
            }
            // (v15 also had a senderContext compare here -- moved to
            // header senderEpoch in v16 PR-FOUNDATION-1b.)
            // 2026-05-25 cross-peer destroy: pass localPlayer so OnDestroy
            // can release a held PHC grab (mainPlayer.grabbing_actor ==
            // doomed) before K2_DestroyActor. Prevents UPhysicsHandle
            // Component::TickComponent reading a dangling GrabbedComponent
            // ptr next frame.
            remote_prop::OnDestroy(p, localPlayer);
            break;
        }
        case net::ReliableKind::EntitySpawn: {
            // Host-broadcast NPC spawn. Validate size + className.len at the
            // trust boundary here; npc_mirror::OnEntitySpawn does the full
            // per-field validation (finite + bounds + allowlist + dedup).
            // UFunction calls inside OnEntitySpawn are game-thread only, so
            // dispatch via GT::Post.
            if (msg.payloadLen < sizeof(net::EntitySpawnPayload)) {
                UE_LOGW("event_feed: EntitySpawn payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::EntitySpawnPayload));
                break;
            }
            // EntitySpawn is host-authoritative. Without a senderPeerSlot
            // trust gate, a malicious client could flood EntitySpawn packets
            // with crafted className strings, forcing R::FindClass
            // GUObjectArray walks on the host's game thread per packet (CPU
            // amplification).
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: EntitySpawn from non-host senderPeerSlot=%d "
                        "-- dropping (NPC sync is host-only)",
                        msg.senderPeerSlot);
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
                ::coop::npc_mirror::OnEntitySpawn(pCopy);
            });
            break;
        }
        case net::ReliableKind::EntityDestroy: {
            // Host-broadcast NPC destroy. Dispatch to npc_mirror::
            // OnEntityDestroy (game-thread UFunction call).
            if (msg.payloadLen < sizeof(net::EntityDestroyPayload)) {
                UE_LOGW("event_feed: EntityDestroy payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::EntityDestroyPayload));
                break;
            }
            // Host-authoritative -- reject non-host senders. A malicious
            // client could otherwise destroy any NPC element id it learned
            // from a legitimate EntitySpawn.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: EntityDestroy from non-host senderPeerSlot=%d "
                        "-- dropping (NPC sync is host-only)",
                        msg.senderPeerSlot);
                break;
            }
            net::EntityDestroyPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            net::EntityDestroyPayload pCopy = p;
            ue_wrap::game_thread::Post([pCopy] {
                ::coop::npc_mirror::OnEntityDestroy(pCopy);
            });
            break;
        }
        case net::ReliableKind::RestoreVitals: {
            // 2026-05-25 LATE +5h (F3 dev key): peer pressed F3 to refill
            // food/sleep/health/coffeePower. No payload to validate -- the
            // action is fixed. Idempotent so an echo bounce is harmless.
            // RestoreVitals is a dev-key path (host presses F3); receiver
            // must enforce host-only origin or any peer could trivially
            // nullify hunger/survival tension.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: RestoreVitals from non-host senderPeerSlot=%d "
                        "-- dropping (host-only dev-key origin)",
                        msg.senderPeerSlot);
                break;
            }
            ue_wrap::game_thread::Post([] { ::coop::dev::restore_vitals::ApplyLocally(); });
            break;
        }
        case net::ReliableKind::PlayerDamage: {
            // vitals Inc3-WIRE: the host detected an enemy hitting THIS peer's puppet
            // and relayed the damage for us to apply to our OWN player. Host-only
            // origin (only the host runs enemies); a client never legitimately sends
            // it, and it is not in the relay whitelist so it is never forwarded. The
            // owner-side targetElementId check + the apply live in player_damage.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: PlayerDamage from non-host senderPeerSlot=%d "
                        "-- dropping (host-only combat origin)", msg.senderPeerSlot);
                break;
            }
            if (msg.payloadLen < sizeof(net::PlayerDamagePayload)) {
                UE_LOGW("event_feed: PlayerDamage payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::PlayerDamagePayload));
                break;
            }
            net::PlayerDamagePayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            coop::player_damage::OnWireDamage(p);
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
            // TeleportClient is host-only. Without a senderPeerSlot trust
            // gate, in a 3-peer session client-slot-1 could craft a
            // TeleportClient targeting client-slot-2 (host's PollGroup
            // fan-out would deliver it) -- a positional griefing/exploit
            // vector at LAN scale, hard exploit at internet scale.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: TeleportClient from non-host senderPeerSlot=%d "
                        "-- dropping (host-only)",
                        msg.senderPeerSlot);
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
            // Reserved flag bits must be zero. A future bit added in v15+
            // would otherwise be silently triggerable by a peer on an older
            // build.
            if (p.flags & ~coop::net::kItemActivateFlag_HasActorKey) {
                UE_LOGW("event_feed: ItemActivate flags=0x%02x has reserved bits "
                        "set -- dropping",
                        static_cast<unsigned>(p.flags));
                break;
            }
            // Intensity + cone angles are passed directly to UE light
            // component setters. Without finite + magnitude
            // checks, a peer can send NaN/Inf (UB inside the renderer)
            // or 1e30 (blinding white screen). Matches the validator
            // pattern every other float-bearing reliable kind uses.
            if (!std::isfinite(p.intensity) ||
                !std::isfinite(p.outerConeAngle) ||
                !std::isfinite(p.innerConeAngle)) {
                UE_LOGW("event_feed: ItemActivate floats non-finite "
                        "(intensity=%.2f outer=%.2f inner=%.2f) -- dropping",
                        p.intensity, p.outerConeAngle, p.innerConeAngle);
                break;
            }
            constexpr float kMaxItemIntensity = 1.0e6f;  // unitless ~10 normal range
            constexpr float kMaxConeAngle     = 180.0f;  // physical degree ceiling
            if (std::fabs(p.intensity) > kMaxItemIntensity ||
                std::fabs(p.outerConeAngle) > kMaxConeAngle ||
                std::fabs(p.innerConeAngle) > kMaxConeAngle) {
                UE_LOGW("event_feed: ItemActivate floats out of bounds "
                        "(intensity=%.2f outer=%.2f inner=%.2f) -- dropping",
                        p.intensity, p.outerConeAngle, p.innerConeAngle);
                break;
            }
            // Self-echo guard via ElementId equality (uint32 compare). The
            // wire field is the SENDER's local Player Element id; if it
            // equals our own local Player Element id, this packet is a
            // loopback bounce. The peer-slot fallback only fires when
            // senderElementId is the 0/unset sentinel (boot/seed
            // pre-handshake sender). With a valid senderElementId, the
            // ElementId compare is authoritative -- gating the peer-slot
            // compare on it prevents an N-peer reassignment race from
            // mis-classifying a legitimate cross-peer packet as a self
            // loopback (e.g. a packet from a peer at slot X arriving before
            // our own AssignPeerSlot reassigned us off slot X).
            const auto selfEid =
                coop::players::Registry::Get().LocalPlayerElementId();
            const bool selfEchoByEid =
                (p.senderElementId != 0u &&
                 p.senderElementId != coop::element::kInvalidId &&
                 selfEid != coop::element::kInvalidId &&
                 p.senderElementId == selfEid);
            const uint8_t selfPeerId = coop::players::Registry::Get().LocalPeerId();
            const bool senderElementIdMissing =
                (p.senderElementId == 0u ||
                 p.senderElementId == coop::element::kInvalidId);
            const bool selfEchoByPeerSlotFallback =
                (senderElementIdMissing &&
                 msg.senderPeerSlot >= 0 &&
                 static_cast<uint8_t>(msg.senderPeerSlot) == selfPeerId);
            if (selfEchoByEid || selfEchoByPeerSlotFallback) {
                UE_LOGI("event_feed: ItemActivate self-echo "
                        "(senderElementId=0x%08x senderPeerSlot=%d via=%s) -- dropping",
                        p.senderElementId, msg.senderPeerSlot,
                        selfEchoByEid ? "eid" : "peerSlot-fallback");
                break;
            }
            // PR-FOUNDATION-1 (2026-05-29): role-range trust boundary on
            // senderElementId. v16 (PR-FOUNDATION-1b) replaces v14's
            // syncContext compare with the Session-layer senderEpoch
            // latch (applied before HandleMessage dispatches here).
            if (!VerifySenderEidRange(msg.senderPeerSlot, p.senderElementId,
                                       "ItemActivate")) {
                break;
            }
            // Resolve senderElementId -> peer slot via Registry::Get. Falls
            // back to msg.senderPeerSlot when the mirror hasn't been
            // established yet (early boot before Join/AssignPeerSlot landed).
            uint8_t resolvedSlot = coop::players::kPeerIdUnknown;
            if (p.senderElementId != 0u &&
                p.senderElementId != coop::element::kInvalidId) {
                auto* el = coop::element::Registry::Get().Get(p.senderElementId);
                if (el && el->GetType() == coop::element::ElementType::Player) {
                    resolvedSlot =
                        static_cast<coop::element::Player*>(el)->PeerSlot();
                }
            }
            if (resolvedSlot >= net::kMaxPeers) {
                if (msg.senderPeerSlot >= 0 &&
                    msg.senderPeerSlot < net::kMaxPeers) {
                    resolvedSlot = static_cast<uint8_t>(msg.senderPeerSlot);
                } else {
                    UE_LOGW("event_feed: ItemActivate could not resolve sender "
                            "slot (senderElementId=0x%08x senderPeerSlot=%d) -- dropping",
                            p.senderElementId, msg.senderPeerSlot);
                    break;
                }
            }
            // The puppet may be null if this packet beat the first
            // PoseSnapshot (puppet spawned lazily on first pose; ItemActivate
            // rides the reliable channel and CAN arrive first under a
            // connect-edge burst). ApplyToPuppetOrDefer stashes the payload
            // when the puppet isn't ready; TickConnect drains it once the
            // puppet appears in the registry.
            //
            // Audit C1 (2026-05-27): capture only resolvedSlot + payload by
            // value into the lambda; re-fetch puppet INSIDE the lambda. The
            // raw void* would risk UAF because Destroy() can run on the
            // game thread between this post and the lambda dispatch,
            // recycling the GUObjectArray slot.
            net::ItemActivatePayload pCopy = p;
            const uint8_t peerSlotCopy = resolvedSlot;
            ue_wrap::game_thread::Post([peerSlotCopy, pCopy] {
                ::coop::RemotePlayer* rp =
                    ::coop::players::Registry::Get().Puppet(peerSlotCopy);
                void* puppetNow = (rp && rp->valid()) ? rp->GetActor() : nullptr;
                ::coop::item_activate::ApplyToPuppetOrDefer(peerSlotCopy, puppetNow, pCopy);
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
            // v13 (A4 2026-05-29): host trust-bound. RedSky is host-only;
            // a non-host senderPeerSlot is a protocol violation.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: RedSky from non-host senderPeerSlot=%d "
                        "(senderElementId=0x%08x) -- dropping",
                        msg.senderPeerSlot, p.senderElementId);
                break;
            }
            // PR-FOUNDATION-1: role-range trust on senderElementId.
            // (v14 syncContext compare replaced by Session-layer senderEpoch in v16.)
            if (!VerifySenderEidRange(msg.senderPeerSlot, p.senderElementId,
                                       "RedSky")) {
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
            // v13 (A4 2026-05-29): host trust-bound. LightningStrike is
            // host-only; a non-host senderPeerSlot is a protocol violation.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: LightningStrike from non-host "
                        "senderPeerSlot=%d (senderElementId=0x%08x) -- dropping",
                        msg.senderPeerSlot, p.senderElementId);
                break;
            }
            // PR-FOUNDATION-1: role-range trust on senderElementId.
            // (v14 syncContext compare replaced by Session-layer senderEpoch in v16.)
            if (!VerifySenderEidRange(msg.senderPeerSlot, p.senderElementId,
                                       "LightningStrike")) {
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
            // v13 (A4 2026-05-29): host trust-bound. WeatherState is
            // host-only; a non-host senderPeerSlot is a protocol violation.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: WeatherState from non-host "
                        "senderPeerSlot=%d (senderElementId=0x%08x) -- dropping",
                        msg.senderPeerSlot, p.senderElementId);
                break;
            }
            // PR-FOUNDATION-1: role-range trust on senderElementId.
            // (v14 syncContext compare replaced by Session-layer senderEpoch in v16.)
            if (!VerifySenderEidRange(msg.senderPeerSlot, p.senderElementId,
                                       "WeatherState")) {
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
            coop::player_handshake::HandleAssignPeerSlot(session, msg);
            break;
        }
        case net::ReliableKind::PlayerJoined: {
            // PR-FOUNDATION Tier 2 T2-1: host-relayed cross-peer identity.
            coop::player_handshake::HandlePlayerJoined(session, msg);
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
