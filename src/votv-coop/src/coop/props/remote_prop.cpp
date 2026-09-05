// coop/remote_prop.cpp -- the PropPose drive (one kinematic drive per peer slot), OnRelease,
// ForceRelease and the per-slot disconnect. The receivers live beside it: PropSpawn in
// remote_prop_spawn.cpp, PropDestroy in remote_prop_destroy.cpp, PropConvert in
// remote_prop_convert.cpp, the reflected physics thunks in remote_prop_physics.cpp.

#include "coop/props/remote_prop.h"
#include "remote_prop_internal.h"  // ResolveLiveActorByEid and DrivePropThrown, shared with the destroy TU

#include "coop/props/active_drive.h"   // the fixed-delay snapshot interp, shared with the trash carry stream
#include "coop/props/prop_sound.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"  // PropMirrors
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_stick_sync.h"  // the stuck wall-attachable gates
#include "coop/props/unresolved_pose_ledger.h"  // a pose-before-spawn race vs a sustained identity gap
#include "coop/element/identity_create.h"  // CreateOrAdoptPropMirror, the prop-mirror bind
#include "coop/props/trash_channel.h"  // the per-eid sync-time context; stale carry and release drops
#include "coop/props/trash_proxy.h"    // the host-authoritative trash mirror
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::remote_prop {

namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// ActiveDrive, BeginLerpToPose, AdvanceLerp, ResetDriveState, LerpAngle, NowMs and the lerp
// constants come from coop/active_drive.h.
using namespace coop::active_drive;

// One drive per peer slot: each client drives its own held prop, so two clients holding
// different props never race.
std::array<coop::active_drive::ActiveDrive, coop::players::kMaxPeers> g_drives{};

// The unstick gate: a PropPose stream aimed at a stuck wall-attachable may be one or two stale
// packets in flight from between the sender's stick commit and its hold-break, which must not
// unstick the mirror. Only a sustained stream (a real re-grab) does: kUnstickStreak consecutive
// fresh poses for one identity inside the window. Per slot, like the drive cache.
struct PendingUnstick {
    void*    actor = nullptr;
    int      streak = 0;
    uint64_t lastMs = 0;
};
std::array<PendingUnstick, coop::players::kMaxPeers> g_pendingUnstick{};
constexpr int      kUnstickStreak   = 5;
constexpr uint64_t kUnstickWindowMs = 400;  // streak resets after this gap (stale burst over)

}  // namespace [drive helpers part 1]

// True when some slot's drive targets `actor`; the spawn receiver skips the transform converge
// for a driven prop (PropPose owns its position).
bool IsActorUnderAnyDrive(void* actor) {
    // g_drives is game-thread only, by assertion rather than a mutex.
    UE_ASSERT_GAME_THREAD("g_drives (IsActorUnderAnyDrive)");
    if (!actor) return false;
    for (const auto& d : g_drives) {
        if (d.actor == actor) return true;
    }
    return false;
}

// The WireKey as a wstring, for the key index.
std::wstring KeyToWString(const coop::net::WireKey& k) {
    std::wstring s;
    s.reserve(k.len);
    for (uint8_t i = 0; i < k.len && i < 31; ++i) {
        s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(k.data[i])));
    }
    return s;
}

namespace {  // [drive helpers part 3]

// The WireKey against the drive's cached key; keys are ASCII, so a byte compare is exact.
bool KeyMatchesCache(int slot, const coop::net::WireKey& k) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return false;
    const auto& d = g_drives[slot];
    if (k.len != d.lastKey.size()) return false;
    return std::memcmp(k.data, d.lastKey.data(), k.len) == 0;
}

// The slot driving the prop with this key, or -1.
int FindSlotByKey(const coop::net::WireKey& k) {
    for (int slot = 0; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
        if (g_drives[slot].actor && KeyMatchesCache(slot, k)) return slot;
    }
    return -1;
}

// Physics on or off for a drive target: an Aprop_C through its StaticMesh, the clump (null mesh)
// through the generic root component. `actor` arrives validated (a fresh resolve or
// drive.LiveActor()); null is a no-op.
void DriveTogglePhysics(void* actor, void* mesh, bool simulate) {
    if (mesh) DriveSimulate(mesh, simulate);
    else if (actor) ue_wrap::engine::SetActorSimulatePhysics(actor, simulate);
}

// Every release-shaped physics re-enable (PropRelease, the stream-stop timeout, the switched-prop
// release) is gated on the stick state: a wall-attachable that stuck while held stays frozen when
// the sender's hold breaks, or the host watches the camera fall off the wall.
bool StickHoldsPhysicsOff(void* actor) {
    return actor && ue_wrap::prop::IsDescendantOfProp(actor) &&
           (ue_wrap::prop::IsFrozen(actor) || ue_wrap::prop::IsStatic(actor));
}

void ResolveAndStartDrive(int slot, const coop::net::PropPoseSnapshot& pose) {
    // A trash carry pose is held unless its ctx is the entity's current generation: a pose ahead of
    // its convert would drive the pre-convert rendering, a stale one after a re-pile or throw would
    // drive the settled pile to the dead clump position. ctx 0 or an unknown eid (a keyed prop) is
    // always fresh.
    if (pose.elementId != 0 &&
        !coop::trash_channel::IsInboundStreamCtxFresh(pose.elementId, pose.ctx, /*requireCurrentGen=*/true)) {
        // One line per (eid, ctx) burst.
        static uint32_t s_lastDropEid = 0; static uint8_t s_lastDropCtx = 0;
        if (pose.elementId != s_lastDropEid || pose.ctx != s_lastDropCtx) {
            UE_LOGI("[PILE] CLIENT HOLD carry pose eid=%u ctx=%u known=%u (not E's current generation -- either "
                    "AHEAD of its convert (would drive the pre-convert OLD rendering -> pile-jump + the triple "
                    "grab-cue) or STALE after a later transition; applies once the matching convert lands. "
                    "known=0 => the ToClump convert was NEVER adopted, so EVERY carry pose holds forever)",
                    pose.elementId, static_cast<unsigned>(pose.ctx),
                    static_cast<unsigned>(coop::trash_channel::CtxForEid(pose.elementId)));
            s_lastDropEid = pose.elementId; s_lastDropCtx = pose.ctx;
        }
        return;
    }
    const std::wstring keyW = KeyToWString(pose.key);
    // Key first (an Aprop_C), then eid (the clump streams key None).
    void* prop = nullptr;
    if (!keyW.empty() && keyW != L"None")
        prop = coop::prop_element_tracker::ResolveLiveActorByKey(keyW);
    if (!prop && pose.elementId != 0)
        prop = ResolveLiveActorByEid(pose.elementId);
    if (!prop) {
        // The ledger separates the ordinary pose-before-spawn race from a sustained stream this
        // peer never got a spawn for: one WARN per sustained stream, not per packet.
        if (coop::unresolved_pose_ledger::Note(slot, keyW, pose.elementId, NowMs())) {
            UE_LOGW("remote_prop: slot %d key '%ls' eid=%u -- SUSTAINED unresolved pose stream "
                    "(>=%u packets over >=%llu ms). This is NOT the ordinary pose-before-spawn "
                    "race: the sender is streaming an item this peer never received a spawn for. "
                    "eid=0 here means the sender had no wire identity for it either (see "
                    "local_streams' carry-only invariant).",
                    slot, keyW.c_str(), pose.elementId,
                    static_cast<unsigned>(coop::unresolved_pose_ledger::kSustainedCount),
                    static_cast<unsigned long long>(coop::unresolved_pose_ledger::kSustainedMs));
        } else {
            UE_LOGI("remote_prop: slot %d incoming PropPose key '%ls' eid=%u -- no local match "
                    "(key or eid); usually the pose overtaking its own spawn broadcast",
                    slot, keyW.c_str(), pose.elementId);
        }
        return;
    }
    coop::unresolved_pose_ledger::Clear(slot, keyW, pose.elementId);
    // A stuck wall-attachable unsticks for an incoming drive only on a sustained stream: the stale
    // poses in flight after the sender's stick commit land here with the drive cache just cleared
    // by OnStickState, and without the streak they would unstick it right back. A static
    // non-attachable never streams legitimately, so it is skipped outright.
    if (ue_wrap::prop::IsDescendantOfProp(prop) &&
        (ue_wrap::prop::IsFrozen(prop) || ue_wrap::prop::IsStatic(prop))) {
        if (!coop::prop_stick_sync::IsWallAttachable(prop)) {
            UE_LOGW("remote_prop: slot %d PropPose for frozen/static non-attachable %p -- ignored", slot, prop);
            return;
        }
        PendingUnstick& pu = g_pendingUnstick[slot];
        const uint64_t now = NowMs();
        if (pu.actor != prop || now - pu.lastMs > kUnstickWindowMs) {
            pu.actor = prop;
            pu.streak = 0;
        }
        pu.lastMs = now;
        if (++pu.streak < kUnstickStreak) return;  // not yet proven a real re-grab
        pu.actor = nullptr;
        pu.streak = 0;
        coop::prop_stick_sync::UnstickForDrive(prop);  // clears flags + simulate(true)/detach
        // Then the drive starts on the freed prop.
    }
    // GetStaticMesh is null for the clump by design (it is driven through the root physics); only
    // an Aprop_C without a mesh is an error.
    void* mesh = ue_wrap::prop::GetStaticMesh(prop);
    if (!mesh && ue_wrap::prop::IsDescendantOfProp(prop)) {
        UE_LOGW("remote_prop: slot %d prop %p (Aprop_C) has null StaticMesh -- cannot drive", slot, prop);
        return;
    }
    UE_LOGI("remote_prop: slot %d GRAB-IN key='%ls' eid=%u -> local actor=%p mesh=%p (%s)",
            slot, keyW.c_str(), pose.elementId, prop, mesh,
            mesh ? "Aprop physics-off" : "clump kinematic (generic physics-off)");
    // Both native grab sounds play only in the grabber's own input chain, so the receiver plays
    // them: the fixed `use` click and the per-material soft cue (a plain-actor clump resolves its
    // root material's physSound row; silent on a miss).
    coop::prop_sound::PlayUseClick(prop);
    coop::prop_sound::PlayGrabSound(prop);
    // Simulation off so the per-packet SetActorLocation sticks: a kinematic follow, the clump
    // through the generic root toggle.
    DriveTogglePhysics(prop, mesh, false);
    g_drives[slot].actor = prop;
    g_drives[slot].actorIdx = R::InternalIndexOf(prop);  // live here; cache for LiveActor()
    g_drives[slot].mesh  = mesh;
    g_drives[slot].lastKey.assign(pose.key.data, pose.key.len);
    g_drives[slot].lastEid = pose.elementId;
    // A host-authoritative trash proxy freezes on a stream gap instead of timing out; it releases
    // only on the explicit reliable edge.
    g_drives[slot].isProxy = (pose.elementId != 0 && coop::trash_proxy::IsProxy(pose.elementId));
    // A fresh identity: the next pose primes (a snap, no drift-in from the rest position), later
    // poses interpolate.
    g_drives[slot].lerpSeeded   = false;
    g_drives[slot].haveTwoSnaps = false;
    // The timeout clock starts now; at zero the 500 ms stream-stop check in Tick would fire on the
    // first late packet after a fresh grab.
    g_drives[slot].lastApplyMs = NowMs();
}

}  // namespace

void Tick(coop::net::Session& session) {
    // Every game-thread tick, from net_pump::Tick.
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::Tick)");
    if (!session.connected()) {
        ForceRelease();
        return;
    }
    // The host reads slots 1..kMaxPeers-1 (its own held prop is published by local_streams, never
    // read back). A client reads every slot but its own: the host's pose arrives stamped slot 0 and
    // the relay stamps a forwarded peer pose with its origin slot (session_relay.cpp).
    const bool isHost = (session.role() == coop::net::Role::Host);
    const int firstSlot = isHost ? 1 : 0;
    const int lastSlot  = static_cast<int>(coop::players::kMaxPeers);
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    const uint64_t nowMs = NowMs();
    for (int slot = firstSlot; slot < lastSlot; ++slot) {
        if (!isHost && static_cast<uint8_t>(slot) == localSlot) continue;
        ActiveDrive& drive = g_drives[slot];
        coop::net::PropPoseSnapshot pose{};
        bool isNew = false;
        const bool have = session.TryGetRemotePropPose(slot, pose, &isNew);
        if (have && isNew) {
            // A first snapshot or a changed identity (key or eid) resolves and switches physics
            // off. The eid check catches a re-grab of a new clump whose key is still None.
            if (!drive.actor || !KeyMatchesCache(slot, pose.key) || drive.lastEid != pose.elementId) {
                if (drive.actor) {
                    // The peer switched props without a Release: the prior body goes back to
                    // physics unless a stick froze it mid-hold.
                    UE_LOGI("remote_prop: slot %d implicit release (peer switched to a new key/eid)", slot);
                    void* liveA = drive.LiveActor();
                    if (!StickHoldsPhysicsOff(liveA))
                        DriveTogglePhysics(liveA, drive.mesh, true);
                    ResetDriveState(drive);
                }
                ResolveAndStartDrive(slot, pose);
            }
            if (drive.LiveActor()) {
                // The pose is the new lerp target; AdvanceLerp moves the actor toward it each tick.
                BeginLerpToPose(drive, ue_wrap::FVector{pose.x, pose.y, pose.z},
                                ue_wrap::FRotator{pose.pitch, pose.yaw, pose.roll}, nowMs);
                drive.lastApplyMs = nowMs;
                // The first 3 and every 60th target per slot.
                static std::array<uint64_t, coop::players::kMaxPeers> sApplyCount{};
                const uint64_t n = ++sApplyCount[slot];
                if (n <= 3 || (n % 60) == 0) {
                    UE_LOGI("remote_prop: slot %d drive #%llu -> target(%.1f, %.1f, %.1f) rot(%.1f, %.1f, %.1f)%s",
                            slot, static_cast<unsigned long long>(n),
                            pose.x, pose.y, pose.z, pose.pitch, pose.yaw, pose.roll,
                            drive.isProxy ? " [proxy]" : "");
                }
            } else if (drive.actor) {
                // The cached actor died (a level unload or GC).
                UE_LOGW("remote_prop: slot %d cached actor no longer live -- dropping drive", slot);
                ResetDriveState(drive);
            }
        }
        // The interpolation advances every tick, pose or no pose: a smooth follow between sends,
        // and a stream gap freezes at the last target.
        AdvanceLerp(drive, nowMs);
        // The stream-stop release, for a non-proxy held item only: 500 ms of silence is a release.
        // A trash proxy freezes through a gap and releases only on the reliable edge (a throw, a
        // ToPile convert, a disconnect), so a hitch mid-walk no longer drops the carried pile.
        if (drive.actor && !drive.isProxy && (nowMs - drive.lastApplyMs) > 500) {
            UE_LOGI("remote_prop: slot %d implicit release (%llu ms since last PropPose)",
                    slot, static_cast<unsigned long long>(nowMs - drive.lastApplyMs));
            void* liveA = drive.LiveActor();
            if (!StickHoldsPhysicsOff(liveA))
                DriveTogglePhysics(liveA, drive.mesh, true);
            ResetDriveState(drive);
        }
    }
}

void OnRelease(int senderSlot, const coop::net::PropReleasePayload& payload, void* localPlayer) {
    // Reads and clears the sender's drive; dispatched from event_feed on the game thread.
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::OnRelease)");
    const std::wstring keyW = KeyToWString(payload.key);
    const float linSpeedSq = payload.linVelX * payload.linVelX +
                             payload.linVelY * payload.linVelY +
                             payload.linVelZ * payload.linVelZ;
    const float linSpeed = std::sqrt(linSpeedSq);
    UE_LOGI("remote_prop: RELEASE wire '%ls' eid=%u ctx=%u linVel=(%.1f, %.1f, %.1f) |v|=%.1f cm/s angVel=(%.1f, %.1f, %.1f) deg/s",
            keyW.c_str(), payload.elementId, static_cast<unsigned>(payload.ctx),
            payload.linVelX, payload.linVelY, payload.linVelZ, linSpeed,
            payload.angVelX, payload.angVelY, payload.angVelZ);
    // A stale trash release (ctx older than the entity's last transition) is dropped, so a throw
    // delayed past a re-pile or re-grab never applies velocity to the re-skinned entity. ctx 0 or
    // eid 0 (a keyed release) is always fresh.
    if (payload.elementId != 0 &&
        !coop::trash_channel::IsInboundStreamCtxFresh(payload.elementId, payload.ctx, /*requireCurrentGen=*/false)) {
        UE_LOGI("[PILE] CLIENT DROP stale release eid=%u ctx=%u (older than E's last transition)",
                payload.elementId, static_cast<unsigned>(payload.ctx));
        return;
    }
    // The releasing peer is the envelope's sender slot, not a key scan: first-match-wins over
    // lastKey would clear the wrong slot when two slots briefly held the same key.
    void* propActor = nullptr;
    void* meshToActOn = nullptr;
    int releasedSlot = -1;
    if (senderSlot >= 0 && senderSlot < static_cast<int>(coop::players::kMaxPeers)) {
        const ActiveDrive& d = g_drives[senderSlot];
        // The slot's drive must carry this key and, for a clump (key None matches any clump), this
        // eid; otherwise the cache is stale and the fresh resolve below applies. Without the eid
        // check a late throw of one clump would land on the next clump the slot grabbed.
        if (d.actor && KeyMatchesCache(senderSlot, payload.key) &&
            (payload.elementId == 0 || d.lastEid == payload.elementId)) {
            releasedSlot = senderSlot;
            propActor = d.LiveActor();  // null if the cached actor died; every apply below then no-ops
            meshToActOn = d.mesh;
        }
    }
    if (releasedSlot < 0) {
        if (void* prop = coop::prop_element_tracker::ResolveLiveActorByKey(keyW)) {
            // No drive entry: a keyed prop resolves from the live world.
            propActor = prop;
            meshToActOn = ue_wrap::prop::GetStaticMesh(prop);
        } else if (payload.elementId != 0) {
            // A clump whose slot already moved on resolves by eid, so its throw still lands on the
            // right entity.
            if (void* prop2 = ResolveLiveActorByEid(payload.elementId)) {
                propActor = prop2;
                meshToActOn = ue_wrap::prop::GetStaticMesh(prop2);
            }
        }
    }
    if (StickHoldsPhysicsOff(propActor)) {
        // The prop stuck while held (PropStickState arrived first on the same reliable lane): no
        // physics re-enable and no velocity, the camera stays on the wall; the drive cache still
        // clears.
        UE_LOGI("remote_prop: RELEASE for stuck wall-attachable %p -- physics stays off (v68)",
                propActor);
        meshToActOn = nullptr;
        propActor = nullptr;
    }
    // A trash proxy throw is not simulated here: local physics would diverge from the host's
    // trajectory and re-enable the collision the proxy runs without. It freezes at the release pose
    // until the host's ToPile convert re-skins it at the landed pile; the swing still plays.
    if (payload.elementId != 0 && coop::trash_proxy::IsProxy(payload.elementId)) {
        if (propActor && linSpeed > coop::net::kThrownLinVelThreshold)
            coop::prop_sound::PlayThrowWhoosh(propActor);
        if (propActor) ClearAnyDriveFor(propActor);  // stop the carry drive; freeze in place
        UE_LOGI("[PILE] CLIENT proxy throw eid=%u |v|=%.1f cm/s -- frozen at release (no local physics); "
                "awaiting host ToPile convert to reposition to the landed pile",
                payload.elementId, linSpeed);
    } else if (meshToActOn) {
        // Simulate first, then the velocities: a kinematic body ignores a velocity write.
        DriveSimulate(meshToActOn, true);
        DriveSetLinearVelocity(meshToActOn, payload.linVelX, payload.linVelY, payload.linVelZ);
        DriveSetAngularVelocity(meshToActOn, payload.angVelX, payload.angVelY, payload.angVelZ);
        // The throw fires above kThrownLinVelThreshold so a passive drop stays silent: the prop's
        // own thrown event (subclass hooks) and the receiver-side swing, since the native swing
        // plays only in the thrower's input chain.
        if (propActor && linSpeed > coop::net::kThrownLinVelThreshold) {
            DrivePropThrown(propActor, localPlayer);
            coop::prop_sound::PlayThrowWhoosh(propActor);
            UE_LOGI("remote_prop: fired Aprop_C.thrown(player=%p) + swing whoosh -- launch speed %.1f cm/s > threshold %.1f",
                    localPlayer, linSpeed, coop::net::kThrownLinVelThreshold);
        }
    } else if (propActor) {  // validated-or-null since assignment (LiveActor / fresh resolve)
        // The clump mirror flies through the generic root physics with PhysicsOnly collision: it
        // falls, lands and rests, but the query facet is off so the host's grab trace cannot pick
        // up the resting mirror in the seconds before the owner's authoritative pile arrives (that
        // grab minted a real clump on top of the pile). The landed pile is a separate spawn with
        // the default collision.
        ue_wrap::engine::SetActorRootCollisionEnabled(propActor, 2 /*PhysicsOnly -- lands but ungrabbable*/);
        ue_wrap::engine::SetActorSimulatePhysics(propActor, true);
        ue_wrap::engine::SetActorRootPhysicsVelocity(
            propActor,
            ue_wrap::FVector{payload.linVelX, payload.linVelY, payload.linVelZ},
            ue_wrap::FVector{payload.angVelX, payload.angVelY, payload.angVelZ});
        UE_LOGI("[PILE] CLIENT applied THROW eid=%u -> clump mirror physics on + velocity |v|=%.1f cm/s (actor=%p) "
                "-- it now flies + lands; the host's LAND convert will re-skin it to a pile",
                payload.elementId, linSpeed, propActor);
        // The swing for the clump too (every throw plays it, whatever the prop), above the same
        // speed gate.
        if (linSpeed > coop::net::kThrownLinVelThreshold) {
            coop::prop_sound::PlayThrowWhoosh(propActor);
        }
    }
    // Only the released slot clears.
    if (releasedSlot >= 0) {
        ResetDriveState(g_drives[releasedSlot]);
    }
}

namespace {

// The wire-received Prop mirrors live in the shared PropMirrors manager beside the tracker's local
// rows: a mirror is bound at the sender's eid (a foreign allocation range) and releases through
// Registry::UnregisterMirror, never FreeId; a converged actor carries both a local row and a
// mirror row, and the ranges keep them apart. Every drain here is mirror-only, so the locals
// survive a reconnect.
using coop::element::PropMirrors;   // canonical accessor (coop/element/mirror_managers.h)

}  // namespace [spawn helpers / mirror-manager]

// The named bind entry (OnSpawn, OnConvert, the kerfur materialise); it forwards to
// CreateOrAdoptPropMirror, the one collision-reconciling create path. A re-bind of the same
// (eid, actor) is a no-op; rebindInPlace is the morph.
void RegisterPropMirror(coop::element::ElementId eid,
                        void* actor,
                        const std::wstring& key,
                        const std::wstring& cls,
                        int senderSlot,
                        bool rebindInPlace) {
    coop::element::CreateOrAdoptPropMirror(eid, actor, key, cls, senderSlot, rebindInPlace);
}

// The eid bound to `actor` among the Prop elements: the mirror-side fallback the chipPile grab
// hook uses when a client grabs a host-owned pile, whose mirror is not in the forward map. O(n),
// on the grab edge only; a snapshot under the manager lock, then a lock-free walk that reads
// GetActor and GetId only.
coop::element::ElementId ResolveMirrorEidByActor(void* actor, bool wireMirrorOnly) {
    if (!actor) return coop::element::kInvalidId;
    std::vector<coop::element::Prop*> snap;
    PropMirrors().Snapshot(snap);
    for (coop::element::Prop* p : snap) {
        if (!p || p->GetActor() != actor) continue;
        // wireMirrorOnly skips the local rows: an actor can carry a local row and a wire row, and
        // the unordered walk picked either.
        if (wireMirrorOnly && !p->IsMirror()) continue;
        return p->GetId();
    }
    return coop::element::kInvalidId;
}

namespace {  // [spawn helpers continued]

// Full-teardown drain, mirrors only: the manager also owns the tracker's local rows, which must
// survive a reconnect (the keyed-prop seed scan runs once per process). Each drained mirror's
// dtor unregisters it.
size_t DrainWirePropMirrors() {
    return PropMirrors().DrainMirrorsOnly();
}


}  // namespace


// A Prop element id to its live actor, or null; the clump is identified this way, its key never
// sticks. Shared with the destroy TU.
void* ResolveLiveActorByEid(uint32_t eid) {
    if (eid == 0 || eid == coop::element::kInvalidId) return nullptr;
    coop::element::Element* e = coop::element::Registry::Get().Get(eid);
    if (!e) return nullptr;
    void* actor = e->GetActor();
    // IsLiveByIndex, not IsLive: the actor is engine-owned and unrooted, so a GC pass since the
    // pose was queued may have freed it; only the cached GUObjectArray slot is read.
    return (actor && R::IsLiveByIndex(actor, e->GetInternalIdx())) ? actor : nullptr;
}

void ClearAnyDriveFor(void* actor) {
    // Every slot's drive on `actor` clears so nothing drives a destroyed actor next tick; the
    // adoption sweep destroys through the same contract.
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::ClearAnyDriveFor)");
    if (!actor) return;
    for (auto& d : g_drives) {
        if (d.actor == actor) {
            UE_LOGI("remote_prop: actor %p was under active kinematic drive (slot %td) -- clearing drive cache",
                    actor, std::distance(&g_drives[0], &d));
            ResetDriveState(d);
        }
    }
}

void ForceRelease() {
    // Every slot's drive clears, on disconnect (from Tick) and on aggregate teardown; a single slot
    // goes through OnDisconnectForSlot.
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::ForceRelease)");
    int released = 0;
    for (auto& d : g_drives) {
        if (!d.actor) continue;
        // A trash proxy retires whole (destroy, unbind, the pin released on the erase), never
        // through ConsumeLocalActor, which would leave a rooted PendingKill actor anchoring its
        // world. RetireProxy clears this drive.
        if (d.isProxy) {
            coop::trash_proxy::RetireProxy(d.lastEid);
            ++released;
            continue;
        }
        // A prop goes back to physics and persists; a clump mirror is destroyed (transient, and its
        // holder's death-watch is gone). IsLiveByIndex: on the quit-to-menu path the world is dying
        // and a recycled slot passes plain IsLive, landing the physics call on a foreign occupant.
        if (R::IsLiveByIndex(d.actor, d.actorIdx)) {
            if (d.mesh) DriveSimulate(d.mesh, true);
            else        ConsumeLocalActor(d.actor);
        }
        ResetDriveState(d);
        ++released;
    }
    if (released > 0) {
        UE_LOGI("remote_prop: force-release on disconnect/teardown (%d active drive(s) cleared)", released);
    }
    // The mirror-only drain; each dtor returns its foreign-range Registry slot.
    const size_t mirrorsDrained = DrainWirePropMirrors();
    if (mirrorsDrained > 0) {
        UE_LOGI("remote_prop: force-release drained %zu wire-received Prop mirror(s)",
                mirrorsDrained);
    }
}

void OnDisconnectForSlot(int peerSlot) {
    // Clears one slot's drive, from net_pump::Tick's per-slot disconnect edge.
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::OnDisconnectForSlot)");
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    // Ledger rows are keyed by slot and slots recycle lowest-free, so the leaver's counts must not
    // reach the next occupant; cleared before the early return, since a slot can have rows without
    // a drive.
    if (const size_t droppedRows = coop::unresolved_pose_ledger::ResetSlot(peerSlot)) {
        UE_LOGI("remote_prop: peer slot %d disconnect -- dropped %zu unresolved-pose row(s)",
                peerSlot, droppedRows);
    }
    // This peer's wire mirrors drain now rather than at full teardown (a rejoin or a recycled eid
    // would collide); mirror-only and owner-slot filtered, before the early return.
    const size_t drainedMirrors = PropMirrors().DrainMirrorsForSlot(peerSlot);
    if (drainedMirrors > 0) {
        UE_LOGI("remote_prop: peer slot %d disconnect -- drained %zu wire prop mirror(s)",
                peerSlot, drainedMirrors);
    }
    ActiveDrive& d = g_drives[peerSlot];
    if (!d.actor) return;
    // A held trash proxy retires whole, as in ForceRelease. Normally
    // trash_proxy::OnDisconnectForSlot already retired it (it runs first in DisconnectSlot) and the
    // drive is empty; RetireProxy is idempotent.
    if (d.isProxy) {
        coop::trash_proxy::RetireProxy(d.lastEid);  // clears this drive via ClearAnyDriveFor
        UE_LOGI("remote_prop: peer slot %d disconnected -- retired held trash proxy eid=%u", peerSlot, d.lastEid);
        return;
    }
    if (d.mesh && R::IsLiveByIndex(d.actor, d.actorIdx)) {
        // A world prop goes back to physics and persists; by-index, for the recycled-slot hazard
        // above.
        DriveSimulate(d.mesh, true);
        UE_LOGI("remote_prop: peer slot %d disconnected -- releasing held prop (key='%s')",
                peerSlot, d.lastKey.c_str());
    } else if (d.mesh) {
        UE_LOGI("remote_prop: peer slot %d disconnected -- held prop already dead (skip release)",
                peerSlot);
    } else {
        // A clump mirror whose holder left mid-carry is destroyed here, since the death-watch that
        // would despawn it lived on the holder; otherwise it lingers as a frozen floating ball.
        ConsumeLocalActor(d.actor);
        UE_LOGI("remote_prop: peer slot %d disconnected mid-carry -- destroyed held clump mirror %p (no leak)",
                peerSlot, d.actor);
    }
    ResetDriveState(d);
}

}  // namespace coop::remote_prop
