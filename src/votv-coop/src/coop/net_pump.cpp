// coop/net_pump.cpp -- see coop/net_pump.h.
//
// Extracted from harness.cpp 2026-05-28 to keep harness.cpp at its
// boot/scenario glue role and bring net_pump.cpp under the modular
// soft cap (audit deferred-plan PR-4.13).

#include "coop/net_pump.h"

#include "coop/event_feed.h"
#include "coop/garbage_sync.h"
#include "coop/grab_observer.h"
#include "coop/item_activate.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/npc_sync.h"
#include "coop/players_registry.h"
#include "coop/prop_lifecycle.h"
#include "coop/prop_snapshot.h"
#include "coop/remote_player.h"
#include "coop/remote_prop.h"
#include "coop/weather_sync.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/hud_feed.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflected_offset.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace coop::net_pump {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// Per-slot remote-player puppets. Indexed by the coop::players::Registry
// slot convention: slot 0 = host (only used on CLIENT processes to hold the
// host's puppet); slots 1..kMaxPeers-1 = clients (used on HOST processes to
// hold each connected client's puppet). On HOST, slot 0 is unused
// (representing host self); on CLIENT, slots 1..kMaxPeers-1 are unused.
// Each entry's actor / spawn-retry / Tick state is independent.
std::array<coop::RemotePlayer, coop::players::kMaxPeers> g_puppets{};

// Cached local mainPlayer_C for the net pump. coop::players::Registry::Get().Local()
// already caches + filters puppets via the controller discriminator
// (per RULE 1 + [[feedback-always-use-user-test-poses]]); we just hold
// a local cache on top to skip the atomic load in the hot pump path.
void* g_netLocal = nullptr;
// Cached controller for the same pawn -- avoids 2 ProcessEvent dispatches per
// pump tick (GetController + GetControlRotation). Bound to g_netLocal's lifetime:
// nulled when g_netLocal is nulled (level change).
void* g_netLocalController = nullptr;

// Connected-state edge detection for the disconnect cleanup (destroy
// the puppet). File-scope (NOT a static-local in Tick) so a session
// restart can reset them explicitly -- otherwise the local-static would
// hold the prior session's value across the new Start.
//
// AGGREGATE flag (g_wasConnected) tracks "any peer connected" -- gates
// global OnDisconnect calls for subsystems with session-wide state
// (weather_sync, prop_lifecycle).
//
// PER-SLOT flags (g_wasConnectedBySlot) track per-peer connection
// edges -- used to destroy the corresponding puppet on per-peer
// disconnect WITHOUT wiping all subsystem state (matters when peer-1
// drops while peer-2 stays connected).
bool g_wasConnected = false;
std::array<bool, coop::players::kMaxPeers> g_wasConnectedBySlot{};

// Held-prop edge detector: file-scope for the same reason as g_wasConnected.
// A static-local would carry a stale prop pointer + key across a session stop/
// restart, causing the next pump to fire SendPropRelease for the OLD session's
// key on the NEW session -- a real bug found by the audit. Cleared by
// OnSessionStart on each session.Start.
void* g_lastHeldProp = nullptr;
coop::net::WireKey g_lastHeldKey{};
uint64_t g_propEmitCount = 0;

// Game thread, ~send-rate: read the local player's pose. Pulled in from
// harness.cpp 2026-05-28; full rationale comments preserved.
bool ReadLocalPose(void* local, void* controller, coop::net::PoseSnapshot& out) {
    if (!local) return false;
    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(local);
    const ue_wrap::FRotator actorRot = ue_wrap::engine::GetActorRotation(local);
    const ue_wrap::FVector vel = ue_wrap::engine::GetActorVelocity(local);
    // BODY yaw: read from the ACTOR (the real body facing direction). Earlier
    // attempt sent controller yaw -- but VOTV's Character body LAGS the camera
    // (head-leads-body is natural to the source), so sending controller yaw made
    // the puppet body show the CAMERA direction instead of the BODY direction
    // = sideways-when-camera-leads (user-confirmed regression).
    // HEAD pitch: read from the CONTROLLER (actor pitch is always 0 on an
    // upright character; the controller carries the real view pitch). Cached
    // by Tick to skip re-resolving GetController every tick.
    out.x = loc.X;
    out.y = loc.Y;
    // Z = source actor.Z (capsule centre on an ACharacter). Receiver
    // reconstructs visible body offset from its own mainPlayer_C's
    // RelLoc.Z at puppet spawn -- same BP class on every peer so the
    // authored constant matches. MTA-fidelity shape (CEntitySA streams
    // matrix.vPos = capsule centre).
    out.z = loc.Z;
    // Normalize yaw and pitch into the canonical FRotator axis range (-180, 180]
    // BEFORE they go on the wire. UE4's AController::GetControlRotation returns
    // RAW ControlRotation unnormalized: looking 10 deg DOWN reads back as
    // Pitch=350. Without this normalize, pitch=350 fails coop::net::ValidatePose's
    // (-90, 90) bound and the ENTIRE packet is dropped on the receiver -- root
    // cause of the "puppet freezes when host looks below horizontal" bug.
    out.yaw = ue_wrap::NormalizeAxis(actorRot.Yaw);
    const ue_wrap::FRotator ctlRot = controller
        ? ue_wrap::engine::GetControlRotation(controller)
        : actorRot;
    out.pitch = ue_wrap::NormalizeAxis(ctlRot.Pitch);
    // headYawDelta: the source's controller-yaw LEAD over its body yaw, in
    // (-180, 180]. The puppet's AnimBP headLookAt yaw reads this so the puppet's
    // head turns to match where the source's CAMERA is looking (free-look /
    // camera-lead-body) -- decoupled from body facing.
    out.headYawDelta = ue_wrap::NormalizeAxis(ctlRot.Yaw - actorRot.Yaw);
    out.speed = std::sqrt(vel.X * vel.X + vel.Y * vel.Y);
    // Pack the source's airborne state. The receiver's BUA-POST observer
    // reads this bit to clear useLegIK on the puppet during the airborne
    // window so the foot-IK trace doesn't plant the puppet's feet to
    // ground while the source is jumping/falling. A-2 (2026-05-29 audit):
    // CMC pointer + MovementMode bytes accessed through ue_wrap::puppet::
    // ReadCharacterIsFalling -- symmetric with the write-side wrapper
    // DriveCharacterMovement (Principle 7: gameplay/coop layer reads
    // engine memory only through ue_wrap). The wrapper also adds IsLive
    // guards on actor + CMC that the prior inline code lacked -- raw
    // deref of a PendingKill mainPlayer_C (e.g. mid level transition)
    // was technically UB; false-positive rate in steady-state gameplay
    // is zero because the possessed mainPlayer_C never carries
    // PendingKill/Unreachable flags.
    out.stateBits = 0;
    if (ue_wrap::puppet::ReadCharacterIsFalling(local)) {
        out.stateBits |= coop::net::kStateBitInAir;
    }
    out._pad[0] = out._pad[1] = out._pad[2] = 0;
    return true;
}

}  // namespace

void InstallObservers(coop::net::Session& session) {
    coop::grab_observer::Install();
    coop::prop_lifecycle::InstallInventory(&session);
    coop::prop_lifecycle::Install(&session);
    coop::npc_sync::Install(&session);
    coop::item_activate::Install(&session);  // Phase 5F flashlight
    coop::weather_sync::Install(&session);   // Phase 5W weather
    coop::garbage_sync::SetSession(&session);
    coop::garbage_sync::Install();           // Phase 5G garbage
    // NOTE: coop::shutdown::Install / UpdateWindowTitle are called from
    // the timeline tick lambda DIRECTLY in harness.cpp -- they MUST NOT
    // be gated on g_netLocal like this function is (HWND subclass +
    // window title must work BEFORE the local player has been possessed,
    // e.g. on the OMEGA splash where the user might X-close before
    // gameplay).
}

void OnSessionStart() {
    g_wasConnected = false;
    g_wasConnectedBySlot.fill(false);
    g_lastHeldProp = nullptr;
    g_lastHeldKey = {};
    g_propEmitCount = 0;
}

coop::RemotePlayer& Puppet(int slot) {
    return g_puppets[slot];
}

void Tick(coop::net::Session& session, float displayOffsetX) {
    // Lazily bring up the on-screen event feed once the GameInstance exists (it
    // is the persistent widget outer). One-time FindObjectByClass until it
    // succeeds, then it stops -- never a per-tick walk after init.
    if (!ue_wrap::hud_feed::IsInitialized()) {
        if (void* gi = R::FindObjectByClass(P::name::GameInstanceClass)) ue_wrap::hud_feed::Init(gi);
    }

    // Detect peer disconnect (Connected -> Handshaking/Disconnected). DESTROY
    // the puppet -- a frozen-in-place puppet of a peer who already quit is
    // confusing and clutters the world. event_feed will also have posted
    // "X left the game". If the peer ever reconnects, Tick auto-spawns a
    // fresh puppet on the first new pose.
    const bool isConnected = (session.state() == coop::net::ConnState::Connected);
    const bool isHost = (session.role() == coop::net::Role::Host);
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        // IsSlotReady (lanes configured) not IsSlotConnected (just has a conn
        // handle): connect-edge replay must wait for ConfigureLanes to land in
        // the Connected callback. Otherwise snapshot fan-out + connect-time
        // broadcasts ship on lane 0 (default) instead of their assigned lane
        // mapping. The disconnect callback clears both flags atomically so
        // the disconnect-edge also fires correctly off IsSlotReady.
        const bool slotConnected = session.IsSlotReady(slot);
        if (g_wasConnectedBySlot[slot] && !slotConnected) {
            // N-3 (2026-05-29 audit): UnregisterPuppet drops the Player Element
            // from players::Registry. If the puppet was never spawned (peer
            // disconnected after Join but before any PoseSnapshot), g_puppets[
            // slot].valid() is false but playerBySlot_[slot] may still hold a
            // mirror Element installed by EstablishMirrorForSlot. Calling
            // UnregisterPuppet unconditionally is safe -- DropPlayerElement_
            // is a no-op when playerBySlot_ is null.
            coop::players::Registry::Get().UnregisterPuppet(static_cast<uint8_t>(slot));
            if (g_puppets[slot].valid()) {
                g_puppets[slot].Destroy();
                UE_LOGI("net: peer slot %d disconnected -- puppet destroyed", slot);
            }
            // Abort any pending/in-progress snapshot drain to this slot so we
            // don't iterate ~1700 candidates calling SendReliableToSlot into a
            // dead connection.
            coop::prop_snapshot::CancelForSlot(slot);
            // Per-slot subsystem cleanup. Only subsystems with actual per-slot
            // state get a call here. prop_lifecycle / npc_sync / weather_sync
            // hold GLOBAL state that the aggregate OnDisconnect below handles
            // correctly on full disconnect.
            coop::remote_prop::OnDisconnectForSlot(slot);
            coop::item_activate::OnDisconnectForSlot(slot);
        }
        if (!g_wasConnectedBySlot[slot] && slotConnected) {
            // Per-slot connect-edge replay.
            // - HOST -> NEW CLIENT (slot 1..3): full snapshot + flashlight +
            //   weather replay so the new client converges to host state.
            // - CLIENT -> HOST (slot 0): announce LOCAL flashlight state so
            //   the host can show it on our puppet. Weather is host-
            //   authoritative; snapshot is host->client only.
            if (isHost && slot >= 1) {
                UE_LOGI("net: peer slot %d connect edge -- replaying snapshot + flashlight + weather", slot);
                coop::prop_snapshot::TriggerForSlot(slot);
                coop::item_activate::QueueConnectBroadcastForSlot(slot);
                coop::weather_sync::QueueConnectBroadcastForSlot(slot);
            } else if (!isHost && slot == 0) {
                UE_LOGI("net: host (slot 0) connect edge -- replaying local flashlight");
                coop::item_activate::QueueConnectBroadcastForSlot(slot);
            }
        }
        g_wasConnectedBySlot[slot] = slotConnected;
    }
    if (g_wasConnected && !isConnected) {
        // Aggregate disconnect (all peers gone). Fire the global OnDisconnect
        // calls -- the per-slot edge block above already handled subsystems
        // with per-slot state, this catches subsystems with session-wide state
        // (weather_sync, npc_sync host-side counter, prop_lifecycle dedupe set).
        // Stashed state belongs to the now-dead session; replaying it on the
        // next session (possibly a different machine after IP change) would
        // carry wrong-peer state. A fresh snapshot enqueues on the next
        // connected edge.
        coop::remote_prop::ForceRelease();
        const auto propStats = coop::prop_lifecycle::OnDisconnect();
        const size_t snapPending = coop::prop_snapshot::OnDisconnect();
        coop::npc_sync::OnDisconnect();
        coop::item_activate::OnDisconnect();
        coop::weather_sync::OnDisconnect();
        UE_LOGI("net: all peers gone -- cleared %zu un-enumerated snapshot candidate(s) + %zu Init-processed entries; takeObjInFlight=0",
                snapPending, propStats.initProcessedDropped);
    }
    g_wasConnected = isConnected;

    // Process up to ~100 snapshot candidates per tick if a snapshot enumeration
    // is in progress (no-op on empty vector).
    if (isConnected) coop::prop_snapshot::DrainChunk();

    // Per-tick drains for subsystems that internally retry until the reliable
    // channel accepts a queued connect-time broadcast (item_activate +
    // weather_sync) and apply any per-peer payloads that arrived BEFORE the
    // corresponding puppet was spawned. Cheap early-return when no pending state.
    coop::item_activate::TickConnect();
    coop::weather_sync::TickConnect();

    if (g_netLocal && !R::IsLive(g_netLocal)) { g_netLocal = nullptr; g_netLocalController = nullptr; }
    if (!g_netLocal) g_netLocal = coop::players::Registry::Get().Local();
    if (g_netLocal) {
        // Re-resolve the controller only when missing or invalidated; the
        // controller pointer stays stable between possess events. Caching here
        // saves ~250 ProcessEvent dispatches/sec at 125 Hz pump.
        if (g_netLocalController && !R::IsLive(g_netLocalController)) g_netLocalController = nullptr;
        if (!g_netLocalController) g_netLocalController = ue_wrap::engine::GetController(g_netLocal);
        // One-shot install of the per-subsystem observers (idempotent).
        InstallObservers(session);
        coop::net::PoseSnapshot mine;
        if (ReadLocalPose(g_netLocal, g_netLocalController, mine)) session.SetLocalPose(mine);

        // Held-prop replication. Read mainPlayer.grabbing_actor; if non-null,
        // build a PropPoseSnapshot from the prop's current world transform and
        // publish to the net thread. On the edge held -> not-held, send a
        // RELIABLE PropRelease so the peer re-enables SimulatePhysics (and we
        // never rely solely on the 500 ms stream-stop timeout).
        void* heldActor = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(g_netLocal) + ue_wrap::reflected_offset::MainPlayer_grabbing_actor());
        // 2026-05-27: chipPile/clump pickup sets mainPlayer.holding_actor INSTEAD
        // of grabbing_actor (their morph path doesn't use PhysicsHandle). Fall
        // back to holding_actor so the PropPose stream covers them too. Offset
        // resolved via reflected_offset so sdk_check catches drift on future
        // VOTV recooks.
        if (!heldActor) {
            const int32_t holdingOff = ue_wrap::reflected_offset::MainPlayer_holding_actor();
            if (holdingOff >= 0) {
                void* maybeHolding = *reinterpret_cast<void**>(
                    reinterpret_cast<uint8_t*>(g_netLocal) + holdingOff);
                if (maybeHolding && R::IsLive(maybeHolding) &&
                    ue_wrap::prop::IsKeyedInteractable(maybeHolding)) {
                    heldActor = maybeHolding;
                }
            }
        }
        if (heldActor && R::IsLive(heldActor)) {
            const std::wstring keyW = ue_wrap::prop::GetInteractableKeyString(heldActor);
            coop::net::PropPoseSnapshot pp{};
            pp.key.len = 0;
            for (size_t i = 0; i < keyW.size() && i < 31; ++i) {
                // The save UUIDs are ASCII; this lossless narrowing is fine.
                pp.key.data[pp.key.len++] = static_cast<char>(keyW[i]);
            }
            const auto loc = ue_wrap::engine::GetActorLocation(heldActor);
            const auto rot = ue_wrap::engine::GetActorRotation(heldActor);
            pp.x = loc.X; pp.y = loc.Y; pp.z = loc.Z;
            // Normalize at the wire boundary: physics-prop rotation accumulates
            // through FQuat<->Euler conversions and can end up at Yaw=359.8 or
            // Pitch=-270; receiver's canonical (-180,180] guard would reject.
            pp.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
            pp.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
            pp.roll  = ue_wrap::NormalizeAxis(rot.Roll);
            session.SetLocalPropPose(true, pp);
            // Throttled emit log: first 3 + every 60th, matches receiver
            // throttle so the two logs can be diff'd line-for-line.
            const uint64_t n = ++g_propEmitCount;
            if (n <= 3 || (n % 60) == 0) {
                UE_LOGI("net: PropPose emit #%llu -> world(%.1f, %.1f, %.1f) rot(%.1f, %.1f, %.1f) key.len=%d",
                        static_cast<unsigned long long>(n),
                        pp.x, pp.y, pp.z, pp.pitch, pp.yaw, pp.roll,
                        static_cast<int>(pp.key.len));
            }
            g_lastHeldProp = heldActor;
            g_lastHeldKey = pp.key;
        } else if (g_lastHeldProp) {
            // Edge: was holding, now not. Stop sending PropPose + tell peer.
            // Read the body's CURRENT linear+angular velocity via
            // prop::GetPhysicsVelocity. By the time this branch runs, the
            // engine has executed the BP graph clearing grabbing_actor, the
            // PHC.ReleaseComponent call, and any post-release AddImpulse the
            // BP issues. PhysX has not stepped yet, so the body still carries
            // the inherited kinematic-tracking velocity ("вжух" mouse-flick
            // launch energy) PLUS any impulse-derived velocity, summed into
            // ONE velocity. Forward both linear + angular so the receiver
            // can SetPhysicsLinearVelocity + SetPhysicsAngularVelocityInDegrees
            // for an identical launch.
            session.SetLocalPropPose(false, {});
            ue_wrap::prop::VelocityState vel{};
            if (R::IsLive(g_lastHeldProp)) {
                vel = ue_wrap::prop::GetPhysicsVelocity(g_lastHeldProp);
            }
            const float linMagSq = vel.linearCmS.X * vel.linearCmS.X +
                                   vel.linearCmS.Y * vel.linearCmS.Y +
                                   vel.linearCmS.Z * vel.linearCmS.Z;
            UE_LOGI("net: held -> released (vel.ok=%d linVel=(%.1f, %.1f, %.1f) |v|=%.1f cm/s angVel=(%.1f, %.1f, %.1f))",
                    vel.ok ? 1 : 0,
                    vel.linearCmS.X, vel.linearCmS.Y, vel.linearCmS.Z,
                    std::sqrt(linMagSq),
                    vel.angularDegS.X, vel.angularDegS.Y, vel.angularDegS.Z);
            session.SendPropRelease(g_lastHeldKey,
                                    vel.linearCmS.X, vel.linearCmS.Y, vel.linearCmS.Z,
                                    vel.angularDegS.X, vel.angularDegS.Y, vel.angularDegS.Z);
            g_lastHeldProp = nullptr;
            g_lastHeldKey = {};
        }
    }

    // Per-slot pose drive. On HOST, iterate slots 1..kMaxPeers-1 (clients).
    // On CLIENT, iterate slot 0 only (the host). Each slot has its own
    // RemotePlayer puppet + spawn-retry backoff.
    {
        using namespace std::chrono;
        // Per-slot spawn retry timer (static-local: harmless across session
        // restarts because the timer is monotonic and a stale "wait until X"
        // either lets us spawn immediately if X is in the past, or makes us
        // wait the bounded remaining time -- no risk of carrying state that
        // affects correctness).
        static std::array<steady_clock::time_point, coop::players::kMaxPeers> sNextSpawnAttempt{};
        const auto now = steady_clock::now();
        const int firstSlot = isHost ? 1 : 0;
        const int lastSlot  = isHost ? coop::players::kMaxPeers : 1;
        // Host self-assigns slot 0 in the registry. Idempotent setter so it's
        // fine to run every tick. Client LocalPeerId is set asynchronously by
        // the wire-layer AssignPeerSlot handler (event_feed.cpp).
        if (isHost) {
            coop::players::Registry::Get().SetLocalPeerId(coop::players::kPeerIdHost);
        }
        for (int slot = firstSlot; slot < lastSlot; ++slot) {
            coop::net::PoseSnapshot remote;
            bool isNew = false;
            if (!session.TryGetRemotePose(slot, remote, &isNew)) continue;
            if (!g_puppets[slot].valid()) {
                // Spawn-retry backoff: BeginDeferredActorSpawnFromClass refuses
                // if the world is mid-transition (OMEGA->story under multi-
                // instance CPU contention can take many seconds; refused
                // spawns reach hundreds/sec at 60 Hz pump rate = pure log
                // noise + wasted reflection calls). Only retry once per second
                // after a failure (RULE 1: don't crutch the engine, just wait).
                if (now < sNextSpawnAttempt[slot]) continue;
                UE_LOGI("net: first remote pose on slot %d -> auto-spawning puppet", slot);
                if (!g_puppets[slot].Spawn()) {
                    UE_LOGW("net: slot %d puppet spawn failed; will retry in 1 s", slot);
                    sNextSpawnAttempt[slot] = now + seconds(1);
                    continue;
                }
                // Register with the central Registry. peerId == slot directly.
                coop::players::Registry::Get().RegisterPuppet(
                    static_cast<uint8_t>(slot), &g_puppets[slot]);
            }
            // Only RE-BASE the interpolation on a NEW packet; re-pushing the
            // latest every frame would zero `errorPos_` mid-window and freeze
            // motion. The per-frame advance happens in Tick() below.
            if (isNew) {
                coop::net::PoseSnapshot withOffset = remote;
                withOffset.x += displayOffsetX;  // loopback mirror shift (0 for real coop)
                g_puppets[slot].SetTargetPose(withOffset);
            }
        }
    }
    // Tick every live puppet (independent of which slot received pose data
    // this frame -- the per-puppet interpolation needs Tick every frame even
    // when no fresh pose arrived).
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        if (g_puppets[slot].valid()) g_puppets[slot].Tick();
    }

    // Receiver-side held-prop driver. Drains the latest PropPose from the
    // session and applies it (lookup-by-Key on first arrival, transform writes
    // thereafter). Stream-stop timeout (>500 ms) treated as implicit release.
    coop::remote_prop::Tick(session);

    // Surface session events (joins/disconnects) to the feed + send our Join.
    // Pass g_netLocal so remote_prop::OnRelease can call Aprop_C.thrown(player)
    // for the natural throw-sound dispatch (Path B in
    // research/findings/votv-throw-sound-path-2026-05-24.md).
    coop::event_feed::Update(session, g_netLocal);

    // Expire old chat-feed lines (10 s TTL) so a "X joined the game" line
    // doesn't linger forever.
    ue_wrap::hud_feed::Tick();
}

}  // namespace coop::net_pump
