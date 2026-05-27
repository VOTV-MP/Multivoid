#include "harness/harness.h"

#include "harness/autotest.h"
#include "harness/config.h"
#include "harness/screenshot.h"
#include "harness/sdk_check.h"
#include "dev/freecam.h"
#include "dev/pos_hud.h"
#include "dev/restore_vitals.h"
#include "dev/teleport_client.h"
#include "coop/event_feed.h"
#include "coop/grab_observer.h"
#include "coop/item_activate.h"
#include "coop/players_registry.h"
#include "coop/nameplate.h"
#include "coop/net/session.h"
#include "coop/npc_sync.h"
#include "coop/prop_lifecycle.h"
#include "coop/prop_snapshot.h"
#include "coop/remote_player.h"
#include "coop/remote_prop.h"
#include "coop/shutdown.h"
#include "coop/weather_sync.h"
#include "ue_wrap/hud_feed.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/reflected_offset.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace harness {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

namespace cfg = harness::config;

// The single coop networking session (Phase 3). Host binds the LAN port;
// client targets the host. Drives the remote puppet from received pose
// snapshots and sends the local player's pose. Off unless a scenario
// starts it.
coop::net::Session g_session;

// Game thread: read the local player's pose into a network snapshot. x/y/z carry
// the source actor's NATIVE world location (capsule centre on a Character) -- the
// engine's own frame, no derived feet-Z or centre-Z arithmetic. The receiver
// reconstructs the visible-body offset at the PUPPET'S ACTOR TRANSFORM (its
// SkeletalMeshActor's mesh comp is the root, so a sub-component RelLoc/RelRot
// shim is impossible). At puppet spawn the receiver measures the local
// mainPlayer's lowest visible bone Z and mesh world transform, derives the
// actor-Z + actor-Yaw additive offsets that ground the visible feet AND
// reconcile the BP-authored mesh-yaw convention, and applies them every
// ApplyToEngine. The wire carries one thing (where the source's actor is),
// one frame -- MTA-style.
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
    // by NetPumpTick to skip re-resolving GetController every tick.
    out.x = loc.X;
    out.y = loc.Y;
    // Z = source actor.Z (capsule centre on an ACharacter). This is UE4's
    // stable physics position -- CMC floor-snap keeps it within ~2 cm of
    // (ground + halfH), it does NOT swing through the +/-85 cm transient
    // that mesh_playerVisible.world.Z does during BP construction / save-load
    // init (Z-trace 2026-05-23 captured `mesh.world.Z = actor.Z + 2.57` for
    // 1 sec post-teleport, then a drop of 84 cm to settled).
    //
    // The receiver does the offset reconstruction at the puppet ACTOR transform:
    // puppet.actor.Z = wire.z + localMeshRelLocZ, where localMeshRelLocZ is the
    // raw USceneComponent::RelativeLocation.Z field (offset +0x11C) read ONCE
    // from the RECEIVER's own mainPlayer_C at puppet spawn. Same BP class on
    // every peer => identical authored RelLoc.Z constant. RULE 1 root-cause
    // fix per the code-architect verdict + MTA fidelity (MTA streams the
    // CEntitySA matrix.vPos = capsule centre, lets the engine reconstruct
    // visible body offset internally -- exactly the same shape).
    //
    // KNOWN LIMITATION (Phase 2 wire bump): when the SOURCE crouches, UE4's
    // ACharacter::Crouch reduces halfH AND adjusts Mesh.RelLoc.Z upward to
    // keep the feet pinned. The LOCAL's cached RelLoc.Z reflects standing
    // state; the puppet's visible mesh will sit ~(crouchedHalfH - standingHalfH)
    // cm too low when the source is crouched. Fix: stream a `bCrouched` bit
    // in PoseSnapshot.flags; receiver applies a different cached offset when
    // crouched. Tracked alongside the ragdoll sync wire change.
    out.z = loc.Z;
    // Normalize yaw and pitch into the canonical FRotator axis range (-180, 180]
    // BEFORE they go on the wire. UE4's AController::GetControlRotation returns
    // the RAW ControlRotation, which the input system accumulates as unnormalized
    // [0, 360): looking 10 deg DOWN reads back as Pitch=350 (not -10). Without
    // this normalize, pitch=350 fails coop::net::ValidatePose's (-90, 90) bound
    // and the ENTIRE packet is dropped on the receiver, freezing position+yaw+
    // everything while the source looks below horizontal -- root cause of the
    // hands-on "puppet freezes when host looks down then teleports on look-up"
    // bug, two converging agents 2026-05-23. Yaw is normalized too: same
    // unnormalized risk, and a normalized yaw keeps RemotePlayer::errorYaw_
    // symmetric around zero for cleaner linear LERP arcs. Mirrors MTA's
    // SCameraRotationSync bWrapInsteadOfClamp wire policy.
    out.yaw = ue_wrap::NormalizeAxis(actorRot.Yaw);
    const ue_wrap::FRotator ctlRot = controller
        ? ue_wrap::engine::GetControlRotation(controller)
        : actorRot;
    out.pitch = ue_wrap::NormalizeAxis(ctlRot.Pitch);
    // headYawDelta: the source's controller-yaw LEAD over its body yaw, in
    // (-180, 180]. The puppet's AnimBP headLookAt yaw component reads this so
    // the puppet's head turns to match where the source's CAMERA is looking
    // (free-look / camera-lead-body) -- decoupled from the body facing, which
    // is what makes the head-track-local-player default look glaringly wrong
    // on a puppet. Normalized for the same wire-boundary reason as yaw/pitch.
    out.headYawDelta = ue_wrap::NormalizeAxis(ctlRot.Yaw - actorRot.Yaw);
    out.speed = std::sqrt(vel.X * vel.X + vel.Y * vel.Y);
    return true;
}

// Runs on the game thread (posted): dump a UFunction's parameter frame so we can
// verify the FProperty offsets (names/offsets/sizes) against the known UE4.27
// signature before we rely on them for marshaling. Temporary validation aid.
void DumpParams(const wchar_t* className, const wchar_t* funcName) {
    void* cls = R::FindClass(className);
    void* fn = cls ? R::FindFunction(cls, funcName) : nullptr;
    if (!fn) {
        UE_LOGW("paramdump: %ls::%ls not found (cls=%p)", className, funcName, cls);
        return;
    }
    const int32_t frame = R::FunctionFrameSize(fn);
    auto params = R::FunctionParams(fn);
    UE_LOGI("paramdump: %ls::%ls  frameSize=%d  params=%zu", className, funcName,
            frame, params.size());
    for (const auto& p : params) {
        UE_LOGI("    %-28ls off=0x%02x size=%-3d flags=0x%llx%s%s", p.name.c_str(),
                p.offset, p.size, static_cast<unsigned long long>(p.flags),
                (p.flags & ue_wrap::profile::cpf::OutParm) ? " OUT" : "",
                (p.flags & ue_wrap::profile::cpf::ReturnParm) ? " RET" : "");
    }
}

// Runs on the game thread (posted): log enough to confirm where we are.
void Report(const char* label) {
    // NumObjects is O(1). Avoid CountObjectsByClass here (a full GUObjectArray
    // walk) -- Report runs in the play path too, and per the post-ship audit we
    // don't pay a 100k-object scan just for a log line.
    const int32_t n = R::NumObjects();
    void* world = R::FindObjectByClass(P::name::WorldClass);
    std::wstring worldName = world ? R::ToString(R::NameOf(world)) : L"(none)";
    UE_LOGI("harness report [%s]: NumObjects=%d, world=%ls", label, n, worldName.c_str());
}

// The C++ orphan (replaces the Lua harness's SpawnOrphan/DriveOrphan): a
// coop::RemotePlayer spawned + pose-driven via our own CallFunction path.
coop::RemotePlayer g_orphan;

// Cached local mainPlayer_C for the net pump. coop::players::Registry::Get().Local()
// already caches + filters puppets via the controller discriminator
// (per RULE 1 + [[feedback-always-use-user-test-poses]]); we just hold
// a local cache on top to skip the atomic load in the hot pump path.
void* g_netLocal = nullptr;
// Cached controller for the same pawn -- avoids 2 ProcessEvent dispatches per
// pump tick (GetController + GetControlRotation). Bound to g_netLocal's lifetime:
// nulled when g_netLocal is nulled (level change).
void* g_netLocalController = nullptr;

// Connected-state edge detection for the disconnect cleanup (Destroy the puppet).
// File-scope (NOT a static-local in NetPumpTick) so a future session restart can
// reset it explicitly via ResetNetState below -- otherwise the local-static would
// hold the prior session's value across the new Start (audit fix).
bool g_wasConnected = false;

// v4 held-prop edge detector: file-scope for the same reason as g_wasConnected.
// A static-local would carry a stale prop pointer + key across a session stop/
// restart, causing the next pump to fire SendPropRelease for the OLD session's
// key on the NEW session -- a real bug found by the audit. Cleared explicitly
// alongside g_wasConnected on each session.Start (post-ship audit 2026-05-24).
void* g_lastHeldProp = nullptr;
coop::net::WireKey g_lastHeldKey{};
uint64_t g_propEmitCount = 0;

// v5 (2026-05-24 post-RE): the v4 throw-impulse cache has been retired per
// RULE 2. The root-cause RE
// (research/findings/votv-throw-release-pipeline-RE-2026-05-24.md) showed the
// dominant launch energy is NOT a discrete AddImpulse call -- it is the
// kinematic-tracking velocity PhysX accumulates while the player flicks the
// camera (Bug B). The release-edge in NetPumpTick now reads the body's
// inherited linear+angular velocity directly via prop::GetPhysicsVelocity
// AFTER the engine has finished release+optional-AddImpulse on the same
// tick, so ONE number captures the full launch state. The
// GrabObserver_PrimComp_AddImpulse stays only as a diagnostic log line (its
// values are no longer cached or shipped).

// ---- Physics-prop pickup observers (Stage 1 of [[project-physics-object-pickup]]) ----
//
// Hook surface: engine-native UPhysicsHandleComponent UFunctions + BP-Timeline
// `grab` auto-functions on mainPlayer_C. The original SDK-header observers
// (smoothGrab/pickupObject/dropGrabObject/throwHoldingProp/switchToHeavyDrag/
// pickupObjectDirect/playerTryToGrab/canPickup) were proven non-dispatching by
// debug build 14d0787 hands-on test 2026-05-23 -- those are BP-pure inline
// functions on mainPlayer_C, so they never appear as ProcessEvent's `function`
// arg. Deleted per RULE 2. Full RE in
// research/findings/votv-physics-interaction-deep-re-2026-05-23.md.
//
// Primary observers (engine-native, universal -- catch every grab path):
//   GrabComponentAtLocation / GrabComponentAtLocationWithRotation  (pickup)
//   SetTargetLocation / SetTargetLocationAndRotation               (per-tick drive)
//   ReleaseComponent                                               (drop, PRE)
//
// Secondary observers (BP-Timeline level, mainPlayer_C scope -- triangulate):
//   InpActEvt_use_K2Node_InputActionEvent_41                       (E press)
//   grab__UpdateFunc (Timeline tick) / grab__FinishedFunc (Timeline end)
//
// The first SetTargetLocation observer fire of a new grab is when we LEARN
// what was grabbed (read mainPlayer.grabbing_actor) and start streaming.
// (Idempotency flag now owned by coop::grab_observer; see coop/grab_observer.cpp.)

// Prop wire-sync state (observers, retry queues, takeObj-in-flight flag,
// processed-Init dedupe set) is owned by coop/prop_lifecycle.cpp;
// this TU calls into it via coop::prop_lifecycle::Install(&g_session) +
// InstallInventory(&g_session) each NetPumpTick, and OnDisconnect() on
// the disconnect edge.

// Phase 5S0 save snapshot bootstrap is owned by coop/prop_snapshot.cpp.
// harness calls SetSession(&g_session) once at boot, Trigger() on the
// host-connected edge, DrainChunk() per NetPumpTick while connected, and
// OnDisconnect() to reset state.

// Retry queues + drainers + IsWireSuppressedPropClass + ProcessedInit set
// + DestroyLocalProp + Init POST / K2_DestroyActor PRE / takeObj PRE+POST
// observers + their installers all moved to coop/prop_lifecycle.cpp
// (2026-05-25 modular refactor; see coop/prop_lifecycle.h).


// Top-level observer orchestrator: retried each NetPumpTick. Each
// subsystem's Install() is idempotent and short-circuits once successful,
// so this is safe to call every tick. The four subsystems are independent
// (a late-loading BP class affects only its own subsystem):
//   - coop::grab_observer::Install -- physics-prop grab/release/throw
//   - coop::prop_lifecycle::InstallInventory -- propInventory_C::takeObj PRE/POST
//   - coop::prop_lifecycle::Install -- Aprop_C::Init POST + K2_DestroyActor PRE
//   - coop::npc_sync::Install -- Phase 5N1 NPC class allowlist + interceptor
void InstallGrabObservers() {
    coop::grab_observer::Install();
    coop::prop_lifecycle::InstallInventory(&g_session);
    coop::prop_lifecycle::Install(&g_session);
    coop::npc_sync::Install(&g_session);
    coop::item_activate::Install(&g_session);  // Phase 5F flashlight
    coop::weather_sync::Install(&g_session);   // Phase 5W weather (host POST observers OR client PRE interceptors per role)
    // NOTE: coop::shutdown::Install / UpdateWindowTitle are called from
    // the timeline tick lambda DIRECTLY (NetPumpTick / play branch /
    // netloopback branch). They MUST NOT be gated on `g_netLocal` like
    // this function is -- the HWND subclass + window title must work
    // BEFORE the local player has been possessed (e.g. on OMEGA splash
    // screen, where the user might X-close before gameplay).
    // Audit-fix 2026-05-26 v3.
}

// Standalone shutdown hooks for the timeline tick. NOT gated on
// g_netLocal -- runs regardless of local-player state. Idempotent.
void TickShutdownHooks() {
    coop::shutdown::Install(&g_session);
    coop::shutdown::UpdateWindowTitle();
}

// Autonomous grab test moved to harness/autotest.cpp.

// Game thread, ~send-rate: push the local player's pose to the session and apply
// the latest remote pose to the puppet (auto-spawning it on the first packet,
// methodology 3.5). `displayOffsetX` shifts the rendered puppet sideways so a
// LOOPBACK mirror (remote pose == our own) is visible next to us; 0 for real coop.
void NetPumpTick(float displayOffsetX) {
    // Lazily bring up the on-screen event feed once the GameInstance exists (it is the
    // persistent widget outer). One-time FindObjectByClass until it succeeds, then it
    // stops -- never a per-tick walk after init (post-ship audit).
    if (!ue_wrap::hud_feed::IsInitialized()) {
        if (void* gi = R::FindObjectByClass(P::name::GameInstanceClass)) ue_wrap::hud_feed::Init(gi);
    }

    // Detect peer disconnect (Connected -> Handshaking/Disconnected). DESTROY the
    // puppet -- a frozen-in-place puppet of a peer who already quit is confusing
    // and clutters the world. event_feed will also have posted "X left the game"
    // (a chat-feed line that now expires via hud_feed::Tick). If the peer ever
    // reconnects, NetPumpTick auto-spawns a fresh puppet on the first new pose.
    const bool isConnected = (g_session.state() == coop::net::ConnState::Connected);
    if (g_wasConnected && !isConnected) {
        // Peer-gone: drop the puppet AND restore physics on any prop the
        // receiver was kinematically driving (without this, a remote prop
        // mid-grab at disconnect would stay frozen in air forever -- the
        // 500 ms stream-stop timeout fires from inside remote_prop::Tick
        // but only if Tick keeps running; relying on it solely is fragile.
        // Audit fix 2026-05-24).
        if (g_orphan.valid()) {
            // Unregister from the central players::Registry BEFORE destroying
            // the actor -- anyone doing a lookup mid-disconnect should get a
            // clean nullptr, not a dangling pointer to a half-destroyed puppet.
            const uint8_t puppetPeerId =
                (g_session.role() == coop::net::Role::Host)
                ? coop::players::kPeerIdHost + 1u
                : coop::players::kPeerIdHost;
            coop::players::Registry::Get().UnregisterPuppet(puppetPeerId);
            g_orphan.Destroy();
        }
        coop::remote_prop::ForceRelease();
        // Audit C-1 (2026-05-24): clear the pending-spawn queue + any
        // half-drained snapshot enumeration. Their contents belong to the
        // now-dead session; shipping them to the NEXT peer (which may be a
        // DIFFERENT machine after IP change) would carry stale or wrong-
        // peer state. A fresh snapshot enqueues on the next connected-edge.
        const auto propStats = coop::prop_lifecycle::OnDisconnect();
        const size_t snapPending = coop::prop_snapshot::OnDisconnect();
        // Phase 5N1 Inc2 (2026-05-25): reset host-side NPC tracking state
        // on disconnect (sessionId counter + tracked map + bypass slot --
        // see coop/npc_sync.cpp OnDisconnect).
        coop::npc_sync::OnDisconnect();
        // Phase 5F Inc5 (2026-05-26): clear any pending connect-replay
        // broadcast + per-peer pending applies. Their contents belong to
        // the now-dead session; reapplying onto the next session's peers
        // (possibly a different machine after IP change) would be wrong.
        coop::item_activate::OnDisconnect();
        // Phase 5W Inc1 (2026-05-26): clear any pending weather broadcast +
        // reset the dedup signature so a fresh connect re-snapshots state.
        coop::weather_sync::OnDisconnect();
        UE_LOGI("net: disconnected -- cleared %zu pending PropSpawn(s) + %zu pending PropDestroy(s) + %zu un-enumerated snapshot candidate(s) + %zu Init-processed entries; takeObjInFlight=0",
                propStats.droppedSpawns, propStats.droppedDestroys, snapPending, propStats.initProcessedDropped);
    }
    // Phase 5S0 (snapshot bootstrap): on every false->true transition of
    // Connected, the host enumerates Aprop_C derivatives and broadcasts
    // each as a PropSpawn. Client receives + de-dupes (skip if local Key
    // already resolves, update transform to converge). Naturally re-fires
    // on every reconnect since g_wasConnected resets in the disconnect
    // branch above. NO trigger from client side (asymmetric: host pushes,
    // client receives).
    if (!g_wasConnected && isConnected) {
        UE_LOGI("net: connected edge -- triggering Phase 5S0 snapshot");
        coop::prop_snapshot::Trigger();
        // Phase 5F Inc5 (2026-05-26): queue our LOCAL flashlight state for
        // the newly-joined peer. Both peers run this on each connect edge
        // -- symmetric: host sends host's state to client puppet, client
        // sends client's state to host puppet. Skipped internally if the
        // local light is OFF (puppet default already matches). The send
        // itself happens in TickConnect() below (reliable channel may be
        // mid-burst from the PropSpawn enumeration; per-tick retry).
        coop::item_activate::QueueConnectBroadcast();
        // Phase 5W Inc1 (2026-05-26): queue our LOCAL weather state for the
        // newly-joined peer. HOST-only sender (no-op on client). Sends
        // current rain/snow/fog/wind state so a client joining mid-storm
        // doesn't see clear weather. Send itself retries per tick.
        coop::weather_sync::QueueConnectBroadcast();
    }
    g_wasConnected = isConnected;

    // Per-tick snapshot work (Phase 5S0 audit I-2): process up to ~100
    // candidates per tick if a snapshot enumeration is in progress.
    // Cheap when no enumeration is active (no-op on empty vector).
    if (isConnected) coop::prop_snapshot::DrainChunk();

    // Phase 5F Inc5 (2026-05-26) per-tick drain. Handles:
    //   - retrying any queued connect-time broadcast until the reliable
    //     channel accepts it
    //   - applying any per-peer ItemActivate payloads that arrived BEFORE
    //     the corresponding puppet was spawned
    // Cheap (early-return) when no pending state. Independent of
    // isConnected: even on disconnect-edge frame TickConnect's broadcast
    // check is a session->connected() guard, and the per-peer apply only
    // runs if a puppet is in the registry (which is cleared on disconnect).
    coop::item_activate::TickConnect();

    // Phase 5W Inc1 (2026-05-26) per-tick weather drain. Retries the
    // host's queued connect-edge weather broadcast until the reliable
    // channel accepts it. Same shape as item_activate's TickConnect.
    coop::weather_sync::TickConnect();

    // Drain any pending PropSpawn payloads that couldn't ship at observer
    // fire time because the reliable channel was busy (audit C-3). Cheap
    // when the queue is empty (mutex lock + size check). Order preserved.
    // Same pattern for PropDestroy (audit I-2 2026-05-24).
    if (isConnected) {
        coop::prop_lifecycle::DrainPendingPropSpawns();
        coop::prop_lifecycle::DrainPendingPropDestroys();
    }

    if (g_netLocal && !R::IsLive(g_netLocal)) { g_netLocal = nullptr; g_netLocalController = nullptr; }
    if (!g_netLocal) g_netLocal = coop::players::Registry::Get().Local();
    if (g_netLocal) {
        // Re-resolve the controller only when missing or invalidated; the
        // controller pointer stays stable between possess events. Caching here
        // saves ~250 ProcessEvent dispatches/sec at 125 Hz pump (post-ship audit).
        if (g_netLocalController && !R::IsLive(g_netLocalController)) g_netLocalController = nullptr;
        if (!g_netLocalController) g_netLocalController = ue_wrap::engine::GetController(g_netLocal);
        // One-shot install of the grab-observer hooks (Stage 1 of [[project-
        // physics-object-pickup]]). Class is reachable now that local is live;
        // idempotent on g_grabObserversInstalled.
        InstallGrabObservers();  // each subsystem's Install is idempotent + retries until ready (audit C-2)
        coop::net::PoseSnapshot mine;
        if (ReadLocalPose(g_netLocal, g_netLocalController, mine)) g_session.SetLocalPose(mine);

        // v4: held-prop replication. Read mainPlayer.grabbing_actor; if non-
        // null, build a PropPoseSnapshot from the prop's current world transform
        // and publish to the net thread. On the edge held -> not-held, send a
        // RELIABLE PropRelease so the peer re-enables SimulatePhysics (and we
        // never rely solely on the 500 ms stream-stop timeout).
        // State is FILE-SCOPE (g_lastHeldProp/Key/g_propEmitCount) -- a
        // static-local would carry stale state across session restart.
        void* heldActor = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(g_netLocal) + ue_wrap::reflected_offset::MainPlayer_grabbing_actor());
        if (heldActor && R::IsLive(heldActor)) {
            const std::wstring keyW = ue_wrap::prop::GetKeyString(heldActor);
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
            // Mirrors PoseSnapshot's NormalizeAxis at ReadLocalPose (audit
            // 2026-05-24).
            pp.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
            pp.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
            pp.roll  = ue_wrap::NormalizeAxis(rot.Roll);
            g_session.SetLocalPropPose(true, pp);
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
            //
            // v5: read the body's CURRENT linear+angular velocity via
            // prop::GetPhysicsVelocity. By the time this branch runs, the
            // engine has already executed (on this tick): the BP graph
            // clearing grabbing_actor, the PHC.ReleaseComponent call, and
            // any post-release AddImpulse the BP issues. PhysX has not
            // stepped yet, so the body still carries the inherited
            // kinematic-tracking velocity (the "вжух" mouse-flick launch
            // energy) PLUS any impulse-derived velocity, summed into ONE
            // velocity. We forward both linear + angular so the receiver
            // can SetPhysicsLinearVelocity + SetPhysicsAngularVelocityInDegrees
            // for an identical launch.
            g_session.SetLocalPropPose(false, {});
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
            g_session.SendPropRelease(g_lastHeldKey,
                                      vel.linearCmS.X, vel.linearCmS.Y, vel.linearCmS.Z,
                                      vel.angularDegS.X, vel.angularDegS.Y, vel.angularDegS.Z);
            g_lastHeldProp = nullptr;
            g_lastHeldKey = {};
        }
    }

    coop::net::PoseSnapshot remote;
    bool isNew = false;
    const bool havePose = g_session.TryGetRemotePose(remote, &isNew);
    if (havePose) {
        if (!g_orphan.valid()) {
            // Spawn-retry backoff: BeginDeferredActorSpawnFromClass refuses if the world
            // is mid-transition (the OMEGA->story transition under 2-instance CPU
            // contention can take many seconds; refused spawns reach hundreds/sec at
            // 60 Hz pump rate -- pure log noise + wasted reflection calls). Only retry
            // once per second after a failure (RULE 1: don't crutch the engine, just
            // wait for it).
            using namespace std::chrono;
            static steady_clock::time_point sNextSpawnAttempt{};
            const auto now = steady_clock::now();
            if (now < sNextSpawnAttempt) return;
            UE_LOGI("net: first remote pose -> auto-spawning remote puppet");
            if (!g_orphan.Spawn()) {
                UE_LOGW("net: remote puppet spawn failed; will retry in 1 s");
                sNextSpawnAttempt = now + seconds(1);
                return;
            }
            // Register with the players::Registry so any subsystem can look
            // up the puppet by peerId (instead of scattered &g_orphan
            // references). 1v1 transitional mapping: the OTHER peer's id is
            // the opposite of our role -- if we're host (id 0), the puppet
            // represents the client (id 1); vice versa. N-peer (4-player
            // target) will switch this to a session-assigned id table.
            const uint8_t puppetPeerId =
                (g_session.role() == coop::net::Role::Host)
                ? coop::players::kPeerIdHost + 1u  // client #1 == 1
                : coop::players::kPeerIdHost;       // the host == 0
            coop::players::Registry::Get().RegisterPuppet(puppetPeerId, &g_orphan);
            coop::players::Registry::Get().SetLocalPeerId(
                (g_session.role() == coop::net::Role::Host)
                ? coop::players::kPeerIdHost
                : static_cast<uint8_t>(coop::players::kPeerIdHost + 1u));
        }
        // Only RE-BASE the interpolation on a NEW packet; re-pushing the latest
        // every frame would zero `errorPos_` mid-window and freeze motion. The
        // per-frame advance happens in Tick() below.
        if (isNew) {
            coop::net::PoseSnapshot withOffset = remote;
            withOffset.x += displayOffsetX;  // loopback mirror shift (0 for real coop)
            g_orphan.SetTargetPose(withOffset);
        }
    }
    if (g_orphan.valid()) g_orphan.Tick();

    // v4: receiver-side held-prop driver. Drains the latest PropPose from the
    // session and applies it (lookup-by-Key on first arrival, transform writes
    // thereafter). Stream-stop timeout (>500 ms) treated as implicit release.
    coop::remote_prop::Tick(g_session);

    // Surface session events (joins/disconnects) to the feed + send our Join.
    // Pass g_netLocal so remote_prop::OnRelease can call Aprop_C.thrown(player)
    // for the natural throw-sound dispatch (Path B in
    // research/findings/votv-throw-sound-path-2026-05-24.md).
    coop::event_feed::Update(g_session, &g_orphan, g_netLocal);

    // Expire old chat-feed lines (10 s TTL) so a "X joined the game" line
    // doesn't linger forever like the early version did.
    ue_wrap::hud_feed::Tick();
}

// Runs on the game thread: log an actor's default subobjects (its components),
// so we can find the mesh component(s) that carry the player's visible body.
void DumpComponents(const char* label, void* actor) {
    if (!actor) { UE_LOGW("components [%s]: null actor", label); return; }
    auto kids = R::ChildObjectsOf(actor);
    UE_LOGI("components [%s] of %p: %zu", label, actor, kids.size());
    for (const auto& k : kids) {
        UE_LOGI("    %-34ls : %ls", k.className.c_str(), k.name.c_str());
    }
}

void Post(GT::Task t) { GT::Post(std::move(t)); }

// Case-insensitive substring test (ASCII keywords against a wide string).
static bool ContainsCI(const std::wstring& hay, const wchar_t* needle) {
    std::wstring h = hay, n = needle;
    auto lower = [](std::wstring& s) { for (auto& c : s) c = static_cast<wchar_t>(::towlower(c)); };
    lower(h); lower(n);
    return h.find(n) != std::wstring::npos;
}

// List the callable UFunctions a class (and its parents) expose, so we can see
// exactly which one to call (e.g. a Proceed/Skip on the OMEGA widget). Functions
// are UObjects of class "Function" whose Outer is one of the classes in the chain.
static void DumpClassFunctions(void* cls, const wchar_t* tag) {
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || R::ClassNameOf(obj) != L"Function") continue;
        void* outer = R::OuterOf(obj);
        void* c = cls;
        for (int d = 0; d < 16 && c; ++d) {
            if (outer == c) {
                UE_LOGI("  %ls fn: %ls", tag, R::ToString(R::NameOf(obj)).c_str());
                break;
            }
            c = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(c) + P::off::UStruct_SuperStruct);
        }
    }
}

// Diagnostic: log every live UUserWidget instance (name + class). Used to find
// the OMEGA WARNING startup widget's class so we can auto-Proceed past it. Fast:
// resolves the UserWidget UClass once and walks each object's super chain by
// POINTER compare (no per-object string work). Any widget whose name/class looks
// like an intro/warning gate is flagged and its UFunctions dumped (to find the
// Proceed/Skip to call next).
void DumpLiveWidgets() {
    void* userWidgetCls = R::FindClass(L"UserWidget");
    if (!userWidgetCls) { UE_LOGW("widgets: UserWidget class not found"); return; }
    static const wchar_t* kGateWords[] = {L"omega", L"warning", L"intro", L"disclaimer",
                                          L"epilepsy", L"splash", L"boot", L"startup",
                                          L"proceed", L"continue", L"title", L"legal"};
    const int32_t n = R::NumObjects();
    int found = 0, flagged = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* c = R::ClassOf(obj);
        bool isWidget = false;
        for (int d = 0; d < 16 && c; ++d) {
            if (c == userWidgetCls) { isWidget = true; break; }
            c = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(c) + P::off::UStruct_SuperStruct);
        }
        if (!isWidget) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;  // skip CDOs
        const std::wstring cn = R::ClassNameOf(obj);
        ++found;
        bool gate = false;
        for (const wchar_t* w : kGateWords)
            if (ContainsCI(nm, w) || ContainsCI(cn, w)) { gate = true; break; }
        if (gate) {
            UE_LOGI("widget[GATE?]: %ls : %ls -- dumping its UFunctions:", cn.c_str(), nm.c_str());
            DumpClassFunctions(R::ClassOf(obj), cn.c_str());
            ++flagged;
        } else {
            UE_LOGI("widget: %ls : %ls", cn.c_str(), nm.c_str());
        }
    }
    UE_LOGI("widgets: %d live UserWidget instances (%d flagged as intro/gate candidates)", found, flagged);
}

// Spawn the 2nd player the INSTANT the local mainPlayer_C exists -- no fixed
// timer. Polls on the game thread (engine state can only be read there) every
// ~100 ms; the moment the local player is present, spawns and returns. The shared
// flag is a shared_ptr so it outlives the worker loop even if a posted check is
// still queued (no use-after-free).
void SpawnSecondPlayerWhenReady() {
    UE_LOGI("play: waiting for STORY gameplay, spawn 2nd player the instant it's ready");
    for (int i = 0; i < 1200; ++i) {  // ~120 s safety cap
        if (coop::shutdown::IsShuttingDown()) {
            UE_LOGI("play: SpawnSecondPlayerWhenReady aborting -- shutdown signaled");
            return;
        }
        auto state = std::make_shared<std::atomic<int>>(0);  // 0 pending,1 not-ready,2 ok,3 failed
        Post([state, i] {
            if (g_orphan.valid()) { state->store(2); return; }
            void* local = coop::players::Registry::Get().Local();
            const bool diag = (i % 20 == 0);  // ~every 2 s
            if (!local) {
                if (diag) {
                    void* w = R::FindObjectByClass(P::name::WorldClass);
                    UE_LOGI("play[wait %d]: no mainPlayer_C; world=%ls objs=%d", i,
                            w ? R::ToString(R::NameOf(w)).c_str() : L"(none)", R::NumObjects());
                }
                state->store(1); return;
            }
            // A mainPlayer_C ALSO exists at the menu (the 'preLoad' world) sitting
            // at the ORIGIN. Spawning against it puts the puppet in the menu world,
            // which the level load then destroys -> "no one spawns". Gate on the
            // player being placed in the real level: a non-origin location.
            const ue_wrap::FVector p = ue_wrap::engine::GetActorLocation(local);
            if (std::abs(p.X) + std::abs(p.Y) + std::abs(p.Z) < 100.f) {
                if (diag) UE_LOGI("play[wait %d]: mainPlayer_C @ORIGIN (%.0f,%.0f,%.0f) -- waiting for real gameplay",
                                  i, p.X, p.Y, p.Z);
                state->store(1);  // still the origin menu player; wait for gameplay
                return;
            }
            UE_LOGI("play: mainPlayer_C ready @ (%.0f,%.0f,%.0f) -- spawning puppet", p.X, p.Y, p.Z);
            state->store(g_orphan.Spawn() ? 2 : 3);
        });
        while (state->load() == 0) ::Sleep(5);  // let the posted check run (~1 frame)
        const int s = state->load();
        if (s == 2) {
            UE_LOGI("play: 2nd player spawned the moment the local player was ready");
            return;
        }
        if (s == 3) UE_LOGW("play: spawn attempt failed; retrying");
        // No sandbox `open` fallback: coop targets STORY mode, loaded via the save
        // system (LoadStorySave), never the sandbox map. We just wait for gameplay.
        ::Sleep(100);  // local player not in world yet -> poll again
    }
    UE_LOGW("play: gave up waiting for local mainPlayer_C");
}

// Boot STORY gameplay (the coop target). LoadStorySave (re)issues `open untitled_1`
// each tick while at preLoad/OMEGA/menu (a single early open during preLoad is
// dropped -> must retry) and returns true once gameplay is reached; ~1.5 s/tick
// throttles the opens. Blocks (worker thread) until loaded or the ~120 s cap.
bool BootStorySaveBlocking() {
    // STORY save slot, from votv-coop.ini "save=<slot>" (defaults s_may2026). Coop
    // targets story mode, so we never boot the sandbox map fresh.
    const std::string slotA = cfg::ReadIniValue("save", "s_may2026");
    const std::wstring slot(slotA.begin(), slotA.end());  // ASCII slot name
    UE_LOGI("harness: target STORY save '%ls'", slot.c_str());
    for (int i = 0; i < 80; ++i) {  // ~120 s cap (boot + omega + level load)
        if (coop::shutdown::IsShuttingDown()) {
            UE_LOGI("harness: BootStorySaveBlocking aborting -- shutdown signaled");
            return false;
        }
        auto st = std::make_shared<std::atomic<int>>(0);  // 0 pending,1 retry,2 ok
        Post([slot, st] { st->store(ue_wrap::engine::LoadStorySave(slot.c_str()) ? 2 : 1); });
        while (st->load() == 0) ::Sleep(5);
        if (st->load() == 2) return true;
        ::Sleep(1500);
    }
    UE_LOGW("harness: did not reach story gameplay in time for '%ls'", slot.c_str());
    return false;
}

// Background timeline. Sleeps for pacing; every engine touch is posted to the
// game thread. Mirrors the Lua harness's newgame timeline.
DWORD WINAPI TimelineThread(LPVOID param) {
    const std::string scenario = *static_cast<std::string*>(param);
    delete static_cast<std::string*>(param);

    UE_LOGI("harness: timeline start, scenario='%s'", scenario.c_str());

    // The OMEGA WARNING is on screen during the FIRST few seconds (the intro/menu
    // world), BEFORE we `open` gameplay. Sample widgets across that window so the
    // dump catches the omega gate (a later single dump only sees gameplay widgets,
    // since VOTV preloads its UMG and the omega widget is gone by then). Each Post
    // runs on the game thread as soon as the pump is live (which is while the omega
    // screen ticks UMG), so these land during the intro.
    const bool storyBoot = (scenario == "play" || scenario == "netloopback");
    if (storyBoot) {
        for (int k = 0; k < 7; ++k) {  // ~2.8 s of coverage
            Post([k] { UE_LOGI("widgets: == intro dump pass %d ==", k); DumpLiveWidgets(); });
            ::Sleep(400);
        }
    } else {
        ::Sleep(8000);  // other scenarios: let the engine fully init
    }
    Post([] { Report("menu"); });
    // Param-offset reflection validator (scenario "paramdump"): logs a UFunction's
    // FProperty layout (names/offsets/sizes/flags) to check against the known
    // signature when bringing up a new function or game build.
    if (scenario == "paramdump") {
        Post([] {
            DumpParams(L"Actor", L"K2_SetActorLocation");
            DumpParams(L"GameplayStatics", L"BeginDeferredActorSpawnFromClass");
            DumpParams(L"GameplayStatics", L"FinishSpawningActor");
        });
    }

    const bool wantGameplay = (scenario == "newgame" || scenario == "orphan" ||
                               scenario == "skin" || scenario == "show" ||
                               scenario == "play" || scenario == "netloopback");
    // Autonomous scenarios boot to the menu then `open` + wait a fixed time. The
    // story-boot scenarios (play/netloopback) load via LoadStorySave in their own
    // branch, so they skip this sandbox `open`.
    if (wantGameplay && !storyBoot) {
        ::Sleep(4000);
        Post([] {
            UE_LOGI("harness: skip-to-gameplay (open %ls)", P::name::GameplayLevel);
            std::wstring cmd = L"open ";
            cmd += P::name::GameplayLevel;
            ue_wrap::engine::ExecuteConsoleCommand(cmd.c_str());
        });
        ::Sleep(25000);  // level load + BeginPlay (mainPlayer_C spawns)
        Post([] { Report("post-load"); });
    }
    // NOTE: in-game HighResShot is BANNED -- it pops a "screenshot saved" toast
    // (bottom-right) that distracts the human tester. For agent-side visual
    // verification use the external tools/capture-window.ps1 (Windows GDI grab,
    // no in-game notification) instead.

    if (scenario == "orphan") {
        // C++ port of the Phase 2.1 orphan derisk: spawn a 2nd mainPlayer_C via
        // our own CallFunction path, confirm the count goes 1->2, pose-drive it
        // by absolute teleport (the network snapshot path), then soak.
        ::Sleep(2000);
        Post([] { Report("pre-spawn"); });
        Post([] {
            UE_LOGI("harness: === spawn coop::RemotePlayer (2nd mainPlayer_C) ===");
            g_orphan.Spawn();
        });
        ::Sleep(2000);
        Post([] { Report("post-spawn"); });
        Post([] {
            if (g_orphan.valid()) {
                ue_wrap::FVector p = g_orphan.GetLocation();
                UE_LOGI("harness: orphan post-spawn pos=(%.0f,%.0f,%.0f)", p.X, p.Y, p.Z);
            }
        });

        // Pose-drive: teleport the orphan in +X steps, read back each time.
        for (int i = 1; i <= 5; ++i) {
            ::Sleep(3000);
            Post([i] {
                if (!g_orphan.valid()) { UE_LOGW("harness: drive %d -- no orphan", i); return; }
                ue_wrap::FVector p = g_orphan.GetLocation();
                p.X += 150.f;
                const bool ok = g_orphan.SetLocation(p);
                ue_wrap::FVector got = g_orphan.GetLocation();
                UE_LOGI("harness: drive step %d set X=%.0f ok=%d -> read (%.0f,%.0f,%.0f)",
                        i, p.X, ok, got.X, got.Y, got.Z);
            });
        }

        ::Sleep(5000);
        Post([] { Report("post-drive soak"); });
        UE_LOGI("harness: ==== AUTONOMOUS ORPHAN TIMELINE DONE ====");
    } else if (scenario == "play") {
        // Hands-on test. Coop targets STORY mode: auto-load the story save via
        // VOTV's own load path (LoadStorySave -> open untitled_1), which also
        // skips the omega/menu (the travel happens as soon as the GameInstance is
        // up). NOT the sandbox `open`. The puppet spawns the instant gameplay live.
        // (Intro widget dumps already ran during the first ~3 s above.)
        BootStorySaveBlocking();
        // Verify the SDK profile resolves against the running VOTV build.
        // Called AFTER BootStorySaveBlocking so VOTV BP classes (loaded on
        // first gameplay-level transition) are present. Logs a one-line
        // summary + per-failure detail; result feeds adaptation when VOTV
        // updates rename/remove content.
        Post([] { harness::sdk_check::Run(); });
        // Coop networking: if votv-coop.ini configures net.role, the puppet is
        // network-driven (auto-spawned on the first peer pose) and we send our pose;
        // otherwise the puppet is spawned locally + static (the pre-net behaviour).
        bool netEnabled = false;
        const coop::net::Config netCfg = cfg::ReadNetConfig(netEnabled);
        if (netEnabled) {
            // Two post-load teleport paths:
            //   * Autonomous-test mode (env VOTVCOOP_AUTOTEST_X/Y/Z/YAW/PITCH set):
            //     position + camera-rotate the local pawn to the role-specific
            //     verified pose so each test instance's screenshot can SEE the
            //     other's puppet (per [[project-autotest-spawn-pose]]).
            //   * Production client mode (no env set, role=Client): land the
            //     client at the КПП guard checkpoint -- user rule 2026-05-23
            //     so both peers spawn near each other in regular play.
            // Either path teleports ONCE post-load, BEFORE session.Start so the
            // very first pose packet already carries the right position.
            const std::string xs = cfg::ReadEnv("VOTVCOOP_AUTOTEST_X");
            const std::string ys = cfg::ReadEnv("VOTVCOOP_AUTOTEST_Y");
            const std::string zs = cfg::ReadEnv("VOTVCOOP_AUTOTEST_Z");
            if (!xs.empty() && !ys.empty() && !zs.empty()) {
                const float ax = static_cast<float>(std::atof(xs.c_str()));
                const float ay = static_cast<float>(std::atof(ys.c_str()));
                const float az = static_cast<float>(std::atof(zs.c_str()));
                const std::string yaws   = cfg::ReadEnv("VOTVCOOP_AUTOTEST_YAW");
                const std::string pitchs = cfg::ReadEnv("VOTVCOOP_AUTOTEST_PITCH");
                const float ayaw   = yaws.empty()   ? 0.f : static_cast<float>(std::atof(yaws.c_str()));
                const float apitch = pitchs.empty() ? 0.f : static_cast<float>(std::atof(pitchs.c_str()));
                // RETRY LOOP: K2_SetActorLocation across large distances on an
                // ACharacter can be silently reverted by VOTV's player constraints
                // (initial diagnostic showed the client's 14-m autotest teleport
                // never stuck while the host's 50-cm one did -- the engine snapped
                // the actor back near the save spawn). K2_TeleportTo is the
                // engine's proper "cross-world teleport" path that bypasses those
                // checks; combine it with a polling retry loop that verifies the
                // actor's actual world position matches the target within 2 m
                // tolerance, since the BP-callable can also lose to a one-frame
                // post-init reposition. Up to 5 s of retries; the loop quits
                // early on success.
                const ue_wrap::FVector target{ax, ay, az};
                const ue_wrap::FRotator targetRot{apitch, ayaw, 0.f};
                bool teleported = false;
                for (int attempt = 0; attempt < 50 && !teleported; ++attempt) {
                    auto okFlag = std::make_shared<std::atomic<int>>(0);  // 0=pending,1=ok,2=nope
                    Post([target, targetRot, ayaw, okFlag] {
                        void* local = coop::players::Registry::Get().Local();
                        if (!local) { okFlag->store(2); return; }
                        ue_wrap::engine::TeleportTo(local, target, targetRot);
                        if (void* ctrl = ue_wrap::engine::GetController(local)) {
                            ue_wrap::engine::SetControlRotation(ctrl, targetRot);
                        }
                        // Verify: read back the actor's world position; success
                        // if within 200 cm of the target on every axis.
                        const auto cur = ue_wrap::engine::GetActorLocation(local);
                        const float dx = cur.X - target.X, dy = cur.Y - target.Y, dz = cur.Z - target.Z;
                        const bool ok = std::fabs(dx) < 200.f && std::fabs(dy) < 200.f && std::fabs(dz) < 200.f;
                        okFlag->store(ok ? 1 : 2);
                    });
                    while (okFlag->load() == 0) ::Sleep(2);
                    teleported = (okFlag->load() == 1);
                    if (!teleported) ::Sleep(100);
                }
                Post([ax, ay, az, ayaw, apitch, teleported] {
                    void* local = coop::players::Registry::Get().Local();
                    const auto cur = local ? ue_wrap::engine::GetActorLocation(local) : ue_wrap::FVector{};
                    UE_LOGI("autotest teleport: target=(%.0f,%.0f,%.0f) yaw=%.1f pitch=%.1f "
                            "-> actual=(%.0f,%.0f,%.0f) settled=%d",
                            ax, ay, az, ayaw, apitch, cur.X, cur.Y, cur.Z, teleported ? 1 : 0);
                });
                ::Sleep(100);
            } else if (netCfg.role == coop::net::Role::Client) {
                Post([] {
                    void* local = coop::players::Registry::Get().Local();
                    if (!local) {
                        UE_LOGW("client KPP teleport: local mainPlayer not in world yet");
                        return;
                    }
                    const ue_wrap::FVector kpp{
                        P::name::kKPPSpawnX,
                        P::name::kKPPSpawnY,
                        P::name::kKPPSpawnZ,
                    };
                    ue_wrap::engine::SetActorLocation(local, kpp);
                    UE_LOGI("client KPP teleport: mainPlayer -> (%.0f,%.0f,%.0f)",
                            kpp.X, kpp.Y, kpp.Z);
                });
                ::Sleep(100);  // let the posted task run before session.Start
            }
            coop::event_feed::SetLocalNickname(cfg::ReadNickname());
            g_wasConnected = false;  // fresh edge-detector for the disconnect cleanup
            // v4: reset held-prop edge state so a session restart doesn't
            // carry a stale prop pointer/key from the prior session (audit
            // fix 2026-05-24).
            g_lastHeldProp = nullptr;
            g_lastHeldKey = {};
            g_propEmitCount = 0;
            coop::prop_lifecycle::SetSession(&g_session);
            coop::npc_sync::SetSession(&g_session);
            coop::prop_snapshot::SetSession(&g_session);
            dev::restore_vitals::SetSession(&g_session);
            dev::teleport_client::SetSession(&g_session);
            g_session.Start(netCfg);
            UE_LOGI("harness: ==== PLAY READY (coop net %s) ====",
                    netCfg.role == coop::net::Role::Host ? "host" : "client");

            // Autonomous grab test: env-gated, runs on BOTH peers. Host drives
            // the full grab/move/release routine through engine native
            // PhysicsHandle UFunctions; client does scan-only (so we can compare
            // Key.ComparisonIndex cross-peer to verify cooked-content FNames are
            // stable -- the hypothesis Stage 4's wire serialization depends on).
            // See harness::autotest::RunAutonomousGrabTest for expected log lines.
            if (cfg::ReadEnv("VOTVCOOP_RUN_GRAB_TEST") == "1") {
                UE_LOGI("harness: VOTVCOOP_RUN_GRAB_TEST=1 (%s) -- spawning grab test thread",
                        netCfg.role == coop::net::Role::Host ? "host" : "client");
                if (HANDLE h = ::CreateThread(nullptr, 0, harness::autotest::GrabTestThread, nullptr, 0, nullptr)) {
                    ::CloseHandle(h);
                }
            }

            // Phase 5F autonomous flashlight test. Both peers run it; each
            // toggles its own local flashlight via reflection 4 times, the
            // POST observer broadcasts each, and the OTHER peer should
            // receive + apply to its puppet. Log diff is the proof.
            if (cfg::ReadEnv("VOTVCOOP_RUN_FLASHLIGHT_TEST") == "1") {
                UE_LOGI("harness: VOTVCOOP_RUN_FLASHLIGHT_TEST=1 (%s) -- spawning flashlight test thread",
                        netCfg.role == coop::net::Role::Host ? "host" : "client");
                if (HANDLE h = ::CreateThread(nullptr, 0, harness::autotest::FlashlightTestThread, nullptr, 0, nullptr)) {
                    ::CloseHandle(h);
                }
            }
            // Phase 5W weather sync autonomous test. Host-only (the routine
            // self-gates on role; client just observes via wire). Forces
            // rain ON/OFF cycles via weather_sync::DebugForceRain which
            // follows the RULE-1 proper invocation sequence (enable_rain
            // precondition + setRainProperties + causeRain +
            // setWindParameters) per the 2026-05-27 RE pass.
            if (cfg::ReadEnv("VOTVCOOP_RUN_WEATHER_TEST") == "1") {
                UE_LOGI("harness: VOTVCOOP_RUN_WEATHER_TEST=1 (%s) -- spawning weather test thread",
                        netCfg.role == coop::net::Role::Host ? "host" : "client");
                if (HANDLE h = ::CreateThread(nullptr, 0, harness::autotest::WeatherTestThread, nullptr, 0, nullptr)) {
                    ::CloseHandle(h);
                }
            }
            // Phase 5W Inc-fix-2 red-sky test (visually unambiguous variant).
            if (cfg::ReadEnv("VOTVCOOP_RUN_REDSKY_TEST") == "1") {
                UE_LOGI("harness: VOTVCOOP_RUN_REDSKY_TEST=1 (%s) -- spawning red sky test thread",
                        netCfg.role == coop::net::Role::Host ? "host" : "client");
                if (HANDLE h = ::CreateThread(nullptr, 0, harness::autotest::RedSkyTestThread, nullptr, 0, nullptr)) {
                    ::CloseHandle(h);
                }
            }

            // Autotest CONTINUED CORRECTION: VOTV's player late-init (BeginPlay /
            // possess / save-restore) can revert the autotest teleport AFTER it
            // appeared to succeed (user-observed: client lands at the correct
            // autotest coords briefly, then VOTV reverts the position once the
            // instance "fully loads"). The fix is to keep re-applying the
            // teleport for ~30 s after session start -- each iteration is a
            // no-op if the actor is already within 200 cm of the target.
            const bool autotestActive = !xs.empty() && !ys.empty() && !zs.empty();
            const float atx = autotestActive ? static_cast<float>(std::atof(xs.c_str())) : 0.f;
            const float aty = autotestActive ? static_cast<float>(std::atof(ys.c_str())) : 0.f;
            const float atz = autotestActive ? static_cast<float>(std::atof(zs.c_str())) : 0.f;
            const float atyaw   = autotestActive ? static_cast<float>(std::atof(cfg::ReadEnv("VOTVCOOP_AUTOTEST_YAW").c_str()))   : 0.f;
            const float atpitch = autotestActive ? static_cast<float>(std::atof(cfg::ReadEnv("VOTVCOOP_AUTOTEST_PITCH").c_str())) : 0.f;
            using namespace std::chrono;
            const auto autotestUntil = steady_clock::now() + seconds(30);

            int tick = 0;
            while (!coop::shutdown::IsShuttingDown()) {
                Post([] { NetPumpTick(0.f); coop::nameplate::Update(); TickShutdownHooks(); });
                // Autotest correction tick: every ~250 ms while still inside the
                // 30 s window, re-check actor position and re-teleport if it
                // drifted outside the 200-cm tolerance. Catches VOTV's late
                // BeginPlay / save-restore reverting our teleport.
                if (autotestActive && steady_clock::now() < autotestUntil && (tick % 30 == 0)) {
                    Post([atx, aty, atz, atyaw, atpitch] {
                        void* local = coop::players::Registry::Get().Local();
                        if (!local) return;
                        const auto cur = ue_wrap::engine::GetActorLocation(local);
                        const float dx = cur.X - atx, dy = cur.Y - aty, dz = cur.Z - atz;
                        if (std::fabs(dx) > 200.f || std::fabs(dy) > 200.f || std::fabs(dz) > 200.f) {
                            const ue_wrap::FVector target{atx, aty, atz};
                            const ue_wrap::FRotator targetRot{atpitch, atyaw, 0.f};
                            ue_wrap::engine::TeleportTo(local, target, targetRot);
                            if (void* ctrl = ue_wrap::engine::GetController(local)) {
                                ue_wrap::engine::SetControlRotation(ctrl, targetRot);
                            }
                            UE_LOGI("autotest correction: was (%.0f,%.0f,%.0f), retargeting (%.0f,%.0f,%.0f)",
                                    cur.X, cur.Y, cur.Z, atx, aty, atz);
                        }
                    });
                }
                // Z-trace tick: every ~500 ms log the local actor.Z AND the
                // local mesh_playerVisible.world.Z (the value the source now
                // streams). This catches the "puppet jumps afloat after init"
                // signature: if the local's mesh.world.Z changes over the
                // first few seconds, the streamed Z changes, the puppet's
                // rendered world Z changes -- the jump moment lives in this
                // trace. Tight cadence (500 ms) for the first 30 s, then drop
                // to the 2 s stats interval.
                using namespace std::chrono;
                static const auto sTickEpoch = steady_clock::now();
                const bool tightTrace = (steady_clock::now() - sTickEpoch) < seconds(30);
                if (tightTrace && (tick % 60 == 0)) {  // ~500 ms at 125 Hz
                    Post([] {
                        void* lp = coop::players::Registry::Get().Local();
                        if (!lp) return;
                        const auto actorLoc = ue_wrap::engine::GetActorLocation(lp);
                        float meshWorldZ = NAN, meshRelZ = NAN;
                        if (void* mc = ue_wrap::puppet::GetMeshPlayerVisibleComponent(lp)) {
                            meshWorldZ = ue_wrap::engine::GetComponentLocation(mc).Z;
                            meshRelZ = ue_wrap::engine::GetComponentRelativeLocation(mc).Z;
                        }
                        // Option G: the wire carries actor.Z; the receiver applies
                        // mesh.RelLoc.Z (raw +0x11C, BP-authored static value). The
                        // raw RelLoc.Z column should STABILIZE within 1-2 sec of
                        // load. If it keeps drifting beyond that, a BP timeline
                        // (wakingup, fixCrouch) is still touching it -- which the
                        // puppet WON'T see, because we don't stream it anymore.
                        UE_LOGI("Z-trace: local actor.Z=%.2f mesh.RelLoc.Z=%.2f mesh.world.Z=%.2f (delta=%.2f)%s",
                                actorLoc.Z, meshRelZ, meshWorldZ, meshWorldZ - actorLoc.Z,
                                g_orphan.valid() ? "" : " (puppet not spawned)");
                        if (g_orphan.valid()) {
                            const auto pp = g_orphan.GetLocation();
                            UE_LOGI("Z-trace: puppet world.Z=%.2f (= wire.actor.Z + cached local mesh.RelLoc.Z)",
                                    pp.Z);
                        }
                    });
                }
                if (++tick % 250 == 0) {  // ~every 2 s at 125 Hz: stats for the LAN test framework
                    Post([] {
                        UE_LOGI("net stats: state=%d sent=%llu recv=%llu puppet=%d",
                                static_cast<int>(g_session.state()),
                                static_cast<unsigned long long>(g_session.packetsSent()),
                                static_cast<unsigned long long>(g_session.packetsRecv()),
                                g_orphan.valid() ? 1 : 0);
                        // Position diagnostic: log the local actor AND the puppet
                        // (if alive) world positions. Lets us see whether the
                        // autotest teleport stuck + whether the pose stream is
                        // updating the puppet position vs leaving it at spawn.
                        if (void* lp = coop::players::Registry::Get().Local()) {
                            const auto loc = ue_wrap::engine::GetActorLocation(lp);
                            const auto rot = ue_wrap::engine::GetActorRotation(lp);
                            ue_wrap::FRotator cRot{};
                            if (void* c = ue_wrap::engine::GetController(lp)) {
                                cRot = ue_wrap::engine::GetControlRotation(c);
                            }
                            UE_LOGI("pos diag: local actor=(%.0f,%.0f,%.0f) actorYaw=%.1f ctrl(P=%.1f Y=%.1f)",
                                    loc.X, loc.Y, loc.Z, rot.Yaw, cRot.Pitch, cRot.Yaw);
                        }
                        if (g_orphan.valid()) {
                            const auto p = g_orphan.GetLocation();
                            UE_LOGI("pos diag: puppet world=(%.0f,%.0f,%.0f)", p.X, p.Y, p.Z);
                        }
                    });
                }
                // 125 Hz pump for MAX SMOOTHNESS: RemotePlayer::Tick advances the
                // interp at game-frame rate (up to ~120 fps), so the 50 ms LERP
                // window resolves to ~6 micro-steps -- no visible stepping. The
                // earlier 60 Hz cap was a workaround for 2-instance same-machine
                // LAN-test CPU starvation; HANDS-ON play is a single host instance
                // (the client runs in a separate game folder = separate process =
                // its own CPU/GPU budget), so the 2-process contention scenario
                // doesn't apply here. Tick is cheap (~3 UFunction writes per call,
                // dirty-gated; SetLocalPose lock + 20-byte copy). If a future
                // multi-instance test surfaces starvation again, the proper fix
                // is a per-frame engine hook -- not dropping the rate.
                ::Sleep(8);
            }
        } else {
            SpawnSecondPlayerWhenReady();
            UE_LOGI("harness: ==== PLAY READY (you have control) ====");
            // (No auto-screenshot here: GDI can't read the 3D swapchain in-process ->
            // black. Use the external tools/capture-window.ps1 for gameplay frames.)
            // Keep the 3D nameplate(s) glued above the head + facing the local player.
            // Also install the Stage-1 grab observers (idempotent) so single-
            // instance hands-on play sees the hook lines fire on E-press --
            // the net branch installs them via NetPumpTick, but this branch
            // doesn't run NetPumpTick. The observers are purely local
            // (observe THIS instance's own UFunctions); zero wire dependency.
            while (!coop::shutdown::IsShuttingDown()) {
                Post([] {
                    InstallGrabObservers();  // each subsystem's Install is idempotent + retries until ready (audit C-2)
                    coop::nameplate::Update();
                    TickShutdownHooks();
                });
                ::Sleep(50);
            }
        }
    } else if (scenario == "netloopback") {
        // Autonomous Phase 3 plumbing test: ONE process, ONE session in loopback
        // (role=host, peer=self, initiate=true). The session sends the local
        // player's pose to itself over real UDP and receives it back; the puppet is
        // auto-spawned on the first packet (3.5) and Drive()n from the round-tripped
        // pose, offset +250 in X so the mirror is visibly beside the player. Proves
        // transport+serialization+session+interp end-to-end without a 2nd machine.
        BootStorySaveBlocking();
        Post([] { harness::sdk_check::Run(); });
        coop::net::Config cfg;
        cfg.role = coop::net::Role::Host;
        cfg.peerIp = "127.0.0.1";
        cfg.initiate = true;  // host targets itself for the loopback handshake
        coop::event_feed::SetLocalNickname(cfg::ReadNickname());
        g_wasConnected = false;  // fresh edge-detector for the disconnect cleanup
        // Same reset as the play branch -- audit fe68c03 missed this site.
        g_lastHeldProp = nullptr;
        g_lastHeldKey = {};
        g_propEmitCount = 0;
        coop::prop_lifecycle::SetSession(&g_session);
        coop::npc_sync::SetSession(&g_session);
        coop::prop_snapshot::SetSession(&g_session);
        dev::restore_vitals::SetSession(&g_session);
        dev::teleport_client::SetSession(&g_session);
        g_session.Start(cfg);
        UE_LOGI("harness: ==== NETLOOPBACK running (self UDP on %u) ====", cfg.port);
        int tick = 0;
        while (!coop::shutdown::IsShuttingDown()) {
            Post([] { NetPumpTick(250.f); coop::nameplate::Update(); TickShutdownHooks(); });
            if (++tick % 120 == 0) {  // ~every 2 s at 60 Hz
                Post([] {
                    UE_LOGI("netloopback: state=%d sent=%llu recv=%llu puppet=%d",
                            static_cast<int>(g_session.state()),
                            static_cast<unsigned long long>(g_session.packetsSent()),
                            static_cast<unsigned long long>(g_session.packetsRecv()),
                            g_orphan.valid() ? 1 : 0);
                });
            }
            ::Sleep(16);  // ~60 Hz pump for smooth Tick() interp (see play-net branch)
        }
    } else if (scenario == "show") {
        // Autonomous visual confirm: spawn the puppet in front, hold idle, then
        // drive a walk (speed) for a few seconds to confirm the AnimBP animates
        // from our direct variable writes. NOTE: this scenario does NOT exercise
        // the receiver-side INTERPOLATION (each SetTargetPose here either snaps
        // -- first call -- or has zero positional delta -- subsequent walk/idle
        // at same loc). The interp linear LERP path is exercised by netloopback
        // and the LAN test.
        ::Sleep(2000);
        Post([] {
            UE_LOGI("show: === spawn skin-puppet ===");
            g_orphan.Spawn();
        });
        ::Sleep(3000);
        Post([] {
            if (!g_orphan.valid()) { UE_LOGW("show: no puppet"); return; }
            const ue_wrap::FVector at = g_orphan.GetLocation();
            UE_LOGI("show: drive WALK in place (speed=200) to test AnimBP locomotion");
            // Same loc/yaw, just bump speed -- the first SetTargetPose since spawn
            // snaps (hasPose_ false), then Tick applies. AnimBP locomotion picks it up.
            coop::net::PoseSnapshot s{at.X, at.Y, at.Z, /*yaw*/0.f, /*pitch*/0.f, /*speed*/200.f};
            g_orphan.SetTargetPose(s);
            g_orphan.Tick();
        });
        ::Sleep(4000);
        Post([] {
            if (!g_orphan.valid()) return;
            const ue_wrap::FVector at = g_orphan.GetLocation();
            coop::net::PoseSnapshot s{at.X, at.Y, at.Z, /*yaw*/0.f, /*pitch*/0.f, /*speed*/0.f};
            g_orphan.SetTargetPose(s);
            g_orphan.Tick();
            UE_LOGI("show: back to idle (speed=0)");
        });
        UE_LOGI("harness: ==== SHOW DONE ====");
    } else if (scenario == "skin") {
        // Investigate the player's visible-body setup: enumerate components of
        // the local pawn and a spawned orphan, and confirm SuperStruct offset.
        ::Sleep(2000);
        Post([] {
            R::DebugProbeSuperStructOffset();
            void* local = coop::players::Registry::Get().Local();
            DumpComponents("local mainPlayer_C", local);
            g_orphan.Spawn();
        });
        ::Sleep(2000);
        Post([] { DumpComponents("orphan mainPlayer_C", g_orphan.actor()); });
        UE_LOGI("harness: ==== SKIN INSPECT DONE ====");
    } else if (scenario == "newgame") {
        ::Sleep(5000);
        Post([] { Report("post-shot"); });
        UE_LOGI("harness: ==== AUTONOMOUS NEWGAME TIMELINE DONE ====");
    } else {
        UE_LOGI("harness: scenario '%s' -- no automatic actions", scenario.c_str());
    }
    return 0;
}

}  // namespace

void Start() {
    // F12 -> screenshot (toast-free, saved to coop-screenshots/). Always on,
    // independent of the scenario, so it's available during hands-on testing.
    screenshot::StartHotkeyWatcher();

    // Dev free-flying camera (HOME toggle). No-op unless votv-coop.ini enables it.
    dev::freecam::Init();

    // Dev pos + camera-rotation overlay (F2 toggle). No-op unless ini enables it.
    dev::pos_hud::Init();

    // Dev F3 (restore stamina/hunger) + F4 (teleport client to host). Both
    // gated by [dev] devkeys=1; no-op otherwise. SetSession was already
    // called above for the play / netloopback paths -- the hotkey threads
    // just acquire it atomically and broadcast on press.
    dev::restore_vitals::Init();
    dev::teleport_client::Init();

    // Install WM_CLOSE subclass on the game HWND so X-close runs our
    // cleanup BEFORE the engine's teardown PE calls fire. Without this
    // the PE detour stays live through UE4 shutdown, faults on
    // half-destroyed UObjects, and the process hangs at 99% RAM. The
    // window might not exist yet at this moment -- Install() retries
    // via TimelineThread's tick loop (see CoopShutdownRetry below).
    coop::shutdown::Install(&g_session);

    auto* scenario = new std::string(cfg::ReadScenario());
    if (HANDLE t = ::CreateThread(nullptr, 0, TimelineThread, scenario, 0, nullptr)) {
        ::CloseHandle(t);
    } else {
        delete scenario;
        UE_LOGE("harness: failed to start timeline thread");
    }
}

}  // namespace harness
