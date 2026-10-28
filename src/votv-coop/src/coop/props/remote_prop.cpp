// coop/props/remote_prop.cpp -- the PropPose tick (one kinematic drive per peer slot), OnRelease,
// ForceRelease and the per-slot disconnect. The receivers live beside it: PropSpawn in
// remote_prop_spawn.cpp, PropDestroy in remote_prop_destroy.cpp, PropConvert in
// remote_prop_convert.cpp; the physics calls are ue_wrap/engine/engine_physics.

#include "coop/props/remote_prop.h"
#include "remote_prop_internal.h"  // impl-private (src-local), NOT under include/
#include "remote_prop_drive.h"     // g_drives, g_holds, the hold gate, ResolveAndStartDrive

#include "coop/dev/swinger_drill.h"     // NotePropPose, NotePropUnresolved
#include "coop/interactables/swinger_join_pose.h"   // Forget: a release supersedes a parked connect pose
#include "coop/props/active_drive.h"   // the fixed-delay snapshot interp, shared with the trash carry stream
#include "coop/props/prop_sound.h"
#include "coop/props/trash_mirror.h"   // Retire (a trash mirror retires whole, pin released after the destroy)
#include "coop/element/bind_wait.h"  // OnBound / MarkDeferred: the bind funnel releases and marks
#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"  // PropMirrors
#include "coop/element/prop.h"
#include "coop/element/quiescence_drain.h"  // SupersedePosCorrection: the release pose is the newest
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/pile_look.h"
#include "coop/props/prop_drive_host.h"    // OverrideClientSyncer: a release that writes the copy ends a client's row on it
#include "coop/props/prop_drive_stream.h"  // IsParked: the host's channel owns a parked copy
#include "coop/props/prop_child_release.h"  // Defer: a jointed door waits for its parent
#include "coop/props/prop_park.h"  // Park / Unpark: a held prop, and a Character welded on it
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_stick_sync.h"  // the stuck wall-attachable gates
#include "coop/props/prop_wire_parity.h"  // ConvergeFrozenSleep, the release's flags
#include "coop/props/unresolved_pose_ledger.h"  // a pose-before-spawn race vs a sustained identity gap
#include "coop/element/identity_create.h"  // CreateOrAdoptPropMirror, the prop-mirror bind
#include "coop/props/trash_channel.h"  // the per-eid sync-time context; stale carry and release drops
#include "coop/interactables/drive_payload_sync.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/swinger.h"
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
// constants come from coop/props/active_drive.h; the drive cache, the hold gate and the drive
// start live in remote_prop_drive.cpp.
using namespace coop::active_drive;

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
        HoldGate& hold = g_holds[slot];
        if (have && isNew && IsHoldClosed(hold, pose.holdGen)) {
            // Poses the holder sent before its release or stick, arriving after it: the ended hold
            // starts nothing. Said once per hold.
            if (hold.tailSaidGen != pose.holdGen) {
                hold.tailSaidGen = pose.holdGen;
                UE_LOGI("remote_prop: slot %d pose of hold %u after that hold closed -- dropped", slot,
                        static_cast<unsigned>(pose.holdGen));
            }
        } else if (have && isNew) {
            // The stream is newest-wins by sequence, so every hold before this pose's is over: the
            // closed floor follows the live one and can never fall half the counter behind it,
            // where a newer hold would read as closed.
            if (pose.holdGen != 0) CloseHold(slot, static_cast<uint16_t>(pose.holdGen - 1));
            // A first snapshot, a changed identity (key or eid) or a new hold resolves and switches
            // physics off. The eid check catches a re-grab of a new clump whose key is still None; the
            // generation, a re-grab of the same prop before the last hold's release has arrived.
            const bool sameProp = drive.actor && KeyMatchesCache(slot, pose.key) && drive.lastEid == pose.elementId;
            const bool newHold = drive.actor && pose.holdGen != hold.driveGen;
            if (!sameProp || newHold) {
                if (drive.actor && !sameProp) {
                    // The peer switched props without a Release: the prior body goes back to
                    // physics unless a stick froze it mid-hold.
                    UE_LOGI("remote_prop: slot %d implicit release (peer switched to a new key/eid)", slot);
                    void* liveA = drive.LiveActor();
                    coop::prop_park::Unpark(liveA, drive.mesh,
                                            !StickHoldsPhysicsOff(liveA) && !HostAuthorsTrashBody(liveA));
                } else if (drive.actor) {
                    UE_LOGI("remote_prop: slot %d hold %u takes the prop of hold %u before that hold's release",
                            slot, static_cast<unsigned>(pose.holdGen), static_cast<unsigned>(hold.driveGen));
                }
                if (drive.actor) ResetDriveState(drive);
                ResolveAndStartDrive(slot, pose);
            }
            if (drive.actor && LocalDragsDoor(drive.LiveActor(), slot, pose.holdGen)) {   // a drag began mid-stream
                coop::prop_park::Unpark(drive.LiveActor(), drive.mesh, /*rootSimulates=*/true);
                ResetDriveState(drive);
            }
            if (drive.LiveActor()) {
                // The pose is the new lerp target; AdvanceLerp moves the actor toward it each tick.
                BeginLerpToPose(drive, ue_wrap::FVector{pose.x, pose.y, pose.z},
                                ue_wrap::FRotator{pose.pitch, pose.yaw, pose.roll}, nowMs);
                drive.lastApplyMs = nowMs;
                coop::dev::swinger_drill::NotePropPose(slot, pose, drive.actor, drive.mesh);
                // The first 3 and every 60th target per slot.
                static std::array<uint64_t, coop::players::kMaxPeers> sApplyCount{};
                const uint64_t n = ++sApplyCount[slot];
                if (n <= 3 || (n % 60) == 0) {
                    UE_LOGI("remote_prop: slot %d drive #%llu -> target(%.1f, %.1f, %.1f) rot(%.1f, %.1f, %.1f)%s",
                            slot, static_cast<unsigned long long>(n),
                            pose.x, pose.y, pose.z, pose.pitch, pose.yaw, pose.roll,
                            drive.isTrashMirror ? " [trash]" : "");
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
        // The stream-stop release, for a held item that is not a trash mirror: 500 ms of silence is a release.
        // A trash mirror freezes through a gap and releases only on the reliable edge (a throw, a
        // ToPile convert, a disconnect), so a hitch mid-walk no longer drops the carried pile.
        if (drive.actor && !drive.isTrashMirror && (nowMs - drive.lastApplyMs) > 500) {
            UE_LOGI("remote_prop: slot %d implicit release (%llu ms since last PropPose)",
                    slot, static_cast<unsigned long long>(nowMs - drive.lastApplyMs));
            void* liveA = drive.LiveActor();
            coop::prop_park::Unpark(liveA, drive.mesh, !StickHoldsPhysicsOff(liveA));
            ResetDriveState(drive);
        }
        // The physics receiver's re-latch (docs/coop-sync-doctrine.md, parking): a driven copy stays
        // kinematic when the game here turns its simulation back on. A flag verb's init() recomputes
        // it from the flags, as this peer's own laptop exit does to a chair another peer carries, and
        // Blueprints call SetSimulatePhysics directly too (the drive's own eject among them), so the
        // state is read each tick rather than one verb hooked: one call per driven prop, a tick's lag.
        // The prop drive only: the clump has no such verb. A door the local player drags is simulating
        // because that drag turned it on, so it is left alone until the next pose's drag check ends the drive.
        if (drive.mesh && drive.LiveActor() && ue_wrap::engine::IsComponentSimulatingPhysics(drive.mesh) &&
            !LocalDragsDoor(drive.LiveActor(), slot, hold.driveGen)) {
            coop::prop_park::Park(drive.LiveActor(), drive.mesh);
            if (++hold.relatches == 1) {
                UE_LOGI("remote_prop: slot %d hold %u -- the game turned simulation back on under the drive; "
                        "re-latched kinematic", slot, static_cast<unsigned>(hold.driveGen));
            } else if (hold.relatches == kRelatchLoud) {
                UE_LOGW("remote_prop: slot %d hold %u -- re-latched %u times: something here keeps switching the "
                        "driven copy's simulation back on", slot, static_cast<unsigned>(hold.driveGen),
                        static_cast<unsigned>(hold.relatches));
            }
        }
    }
}

void CloseHold(int slot, uint16_t holdGen) {
    UE_ASSERT_GAME_THREAD("g_holds (remote_prop::CloseHold)");
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers) || holdGen == 0) return;
    HoldGate& h = g_holds[slot];
    if (h.haveClosed && !GenAfter(holdGen, h.closedGen)) return;  // an older close changes nothing
    h.closedGen = holdGen;
    h.haveClosed = true;
}

void OnRelease(int senderSlot, const coop::net::PropReleasePayload& payload, void* localPlayer) {
    // Reads and clears the sender's drive; dispatched from event_feed on the game thread.
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::OnRelease)");
    const std::wstring keyW = KeyToWString(payload.key);
    coop::swinger_join_pose::Forget(keyW);   // a release is newer than a connect pose parked for this lid
    const float linSpeedSq = payload.linVelX * payload.linVelX +
                             payload.linVelY * payload.linVelY +
                             payload.linVelZ * payload.linVelZ;
    const float linSpeed = std::sqrt(linSpeedSq);
    UE_LOGI("remote_prop: RELEASE wire '%ls' eid=%u ctx=%u hold=%u flags=0x%02x linVel=(%.1f, %.1f, %.1f) "
            "|v|=%.1f cm/s angVel=(%.1f, %.1f, %.1f) deg/s",
            keyW.c_str(), payload.elementId, static_cast<unsigned>(payload.ctx),
            static_cast<unsigned>(payload.holdGen), static_cast<unsigned>(payload.physFlags),
            payload.linVelX, payload.linVelY, payload.linVelZ, linSpeed,
            payload.angVelX, payload.angVelY, payload.angVelZ);
    // The release closes its hold, even one dropped as stale below, so a pose of it still in flight
    // starts nothing.
    CloseHold(senderSlot, payload.holdGen);
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
        const bool driveHasIt = d.actor && KeyMatchesCache(senderSlot, payload.key) &&
                                (payload.elementId == 0 || d.lastEid == payload.elementId);
        if (driveHasIt && payload.holdGen != 0 && GenAfter(g_holds[senderSlot].driveGen, payload.holdGen)) {
            // The holder took the prop again before this release arrived: its new hold owns it.
            UE_LOGI("remote_prop: RELEASE of hold %u after hold %u took the same prop -- nothing to apply",
                    static_cast<unsigned>(payload.holdGen), static_cast<unsigned>(g_holds[senderSlot].driveGen));
            return;
        }
        if (driveHasIt) {
            releasedSlot = senderSlot;
            propActor = d.LiveActor();  // null if the cached actor died; every apply below then no-ops
            meshToActOn = d.mesh;
        }
    }
    if (releasedSlot < 0) {
        void* prop = ResolveKeyedActor(keyW);
        if (prop) {
            // No drive entry carries this key and eid: a keyed prop resolves from the live world.
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
    // Someone else moves the prop -- this peer's own player, another slot's drive, the host's
    // driven-prop channel parking it: that owner has its physics and its flags, and this release
    // closes its own hold, clears its own drive and moves nothing. This slot's drive left the copy
    // kinematic, which this peer's own grab cannot hold, so a grab here gets its body back.
    const bool localGrab = propActor && localPlayer && ue_wrap::engine::IsMainPlayerGrabbing(localPlayer, propActor);
    const bool otherOwner = propActor && (IsActorDrivenByOtherThan(propActor, releasedSlot) ||
                                          coop::prop_drive_stream::IsParked(propActor));
    if (localGrab || otherOwner) {
        UE_LOGI("remote_prop: RELEASE of hold %u for %p, which %s has now -- nothing to apply",
                static_cast<unsigned>(payload.holdGen), propActor,
                otherOwner ? "another drive" : "this peer's own grab");
        if (releasedSlot >= 0) {
            if (localGrab && !otherOwner)
                coop::prop_park::Unpark(propActor, meshToActOn,
                                        !StickHoldsPhysicsOff(propActor) && !HostAuthorsTrashBody(propActor));
            ResetDriveState(g_drives[releasedSlot]);
        }
        return;
    }
    // The release writes the copy's pose and physics below: a client that syncs the prop loses it first, as to a host
    // verb. A no-op off the host and for a prop with no client row.
    if (propActor) coop::prop_drive_host::OverrideClientSyncer(propActor, "client release");
    // A wall-attachable already stuck here was stuck by its stick, which arrived first on the same lane,
    // and the component's own re-trace placed it; read before the converge below, which freezes a
    // copy whose stick did not land here and must not be taken for one.
    const bool stuckOnWall = StickHoldsPhysicsOff(propActor) && coop::prop_stick_sync::IsWallAttachable(propActor);
    // The holder's frozen and sleep at the release edge, before the physics decision below reads
    // them. A grab that reached this copy through no pose still ends unfrozen here, and a hold that
    // ended in a drive slot or a mount ends frozen here too. The host authors a trash body's physics.
    if (propActor && !HostAuthorsTrashBody(propActor))
        coop::prop_wire_parity::ConvergeFrozenSleep(propActor, payload.physFlags);
    // Where the holder's copy was at the edge: this copy lets go from there even when the hold's last
    // poses were lost, or none arrived, and a hold that ended frozen stays where it ended, which is
    // where the holder's slot or mount put it -- all but a copy its stick placed. The host authors a
    // trash body's place.
    if (propActor && payload.hasPose && !HostAuthorsTrashBody(propActor) && !stuckOnWall) {
        ue_wrap::engine::SetActorLocation(propActor, ue_wrap::FVector{payload.locX, payload.locY, payload.locZ});
        ue_wrap::engine::SetActorRotation(propActor,
                                          ue_wrap::FRotator{payload.rotPitch, payload.rotYaw, payload.rotRoll});
        // The registry reverse, not GetPropElementIdForActor (locals only), and not payload.elementId
        // (0 for a keyed prop): a correction armed on this copy is keyed by the host's eid.
        const coop::element::ElementId heldEid = coop::element::Registry::Get().EidForActor(propActor);
        if (heldEid != coop::element::kInvalidId && heldEid != 0u)
            coop::element::quiescence_drain::SupersedePosCorrection(heldEid, "hold release");
    }
    if (StickHoldsPhysicsOff(propActor)) {
        // The prop froze while held -- a wall-attachable's stick, a slot's insert, a mount: no
        // physics re-enable and no velocity, it stays where it froze; the drive cache still clears,
        // and a Character the park stopped moves again.
        UE_LOGI("remote_prop: RELEASE for a prop frozen or static here %p -- physics stays off",
                propActor);
        coop::prop_park::Unpark(propActor, meshToActOn, /*rootSimulates=*/false);
        meshToActOn = nullptr;
        propActor = nullptr;
    }
    // A thrown trash mirror is not simulated here: local physics would diverge from the host's
    // trajectory, and the host streams the clump's flight as poses until it re-piles. It freezes at
    // the release pose until that stream or the host's ToPile convert moves it; the swing plays.
    if (propActor && HostAuthorsTrashBody(propActor)) {
        if (propActor && linSpeed > coop::net::kThrownLinVelThreshold)
            coop::prop_sound::PlayThrowWhoosh(propActor);
        if (propActor) ClearAnyDriveFor(propActor);  // stop the carry drive; freeze in place
        UE_LOGI("[PILE] CLIENT trash throw eid=%u |v|=%.1f cm/s -- frozen at release (no local physics); "
                "the host's pose stream and its ToPile convert place it",
                payload.elementId, linSpeed);
    } else if (meshToActOn && ue_wrap::swinger::IsSwinger(propActor)) {
        // A door: no throw. Jointed to a parent the stream still has parked, it is let go after the parent
        // (prop_child_release.h), not dragged by it.
        void* const parent = E::ParentActorOf(propActor);
        if (parent && coop::prop_drive_stream::IsParked(parent))
            coop::prop_child_release::Defer(propActor, parent);
        else {
            coop::prop_child_release::Forget(propActor);   // a wait left by an earlier release is stale
            coop::prop_park::Unpark(propActor, meshToActOn, /*rootSimulates=*/true);
        }
    } else if (meshToActOn) {
        // Simulate first, then the velocities: a kinematic body ignores a velocity write.
        coop::prop_park::Unpark(propActor, meshToActOn, /*rootSimulates=*/true);
        ue_wrap::engine::SetComponentLinearVelocity(meshToActOn, payload.linVelX, payload.linVelY, payload.linVelZ);
        ue_wrap::engine::SetComponentAngularVelocity(meshToActOn, payload.angVelX, payload.angVelY, payload.angVelZ);
        // The throw fires above kThrownLinVelThreshold so a passive drop stays silent: the prop's
        // own thrown event (subclass hooks) and the receiver-side swing, since the native swing
        // plays only in the thrower's input chain.
        if (propActor && linSpeed > coop::net::kThrownLinVelThreshold) {
            ue_wrap::prop::CallPropThrown(propActor, localPlayer);
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
        coop::prop_park::Unpark(propActor, nullptr, /*rootSimulates=*/true);
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
    // A pile bound after its look arrived takes the look here (coop/props/pile_look.h) -- when the
    // bind took: the create path can refuse one (a steal across rows, a 1:1 conflict, the host
    // authority wall), and then `actor` is not this eid's and must not wear its look.
    auto* row = coop::element::Registry::Get().Get(eid);
    if (row && row->GetActor() == actor) {
        coop::pile_look::OnBound(static_cast<uint32_t>(eid), actor);
        coop::drive_payload_sync::OnBound(static_cast<uint32_t>(eid), actor, senderSlot);  // a data drive's row, by its author
        coop::element::bind_wait::OnBound(static_cast<uint32_t>(eid));  // messages held for this eid replay at the next Pump
    } else if (!row && actor && eid != 0 && eid != coop::element::kInvalidId) {
        // Refused and rowless: no retry runs inside the create path, so messages for the eid wait here until a
        // later bind, a destroy of it, or the world change.
        coop::element::bind_wait::MarkDeferred(static_cast<uint32_t>(eid), "the create path refused the bind");
    }
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

void EndAnyHoldOn(void* actor) {
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::EndAnyHoldOn)");
    if (!actor) return;
    for (int s = 0; s < static_cast<int>(coop::players::kMaxPeers); ++s) {
        ActiveDrive& d = g_drives[s];
        if (d.actor != actor) continue;
        CloseHold(s, g_holds[s].driveGen);
        UE_LOGI("remote_prop: slot %d hold %u ends where the prop was taken from the hand (%p)", s,
                static_cast<unsigned>(g_holds[s].driveGen), actor);
        ResetDriveState(d);
    }
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
    // A slot's generations are its holder's; the next session's holder counts its own.
    g_holds.fill({});
    ResetWireDestroys();
    int released = 0;
    for (auto& d : g_drives) {
        if (!d.actor) continue;
        // A trash mirror retires whole (drives evicted, destroyed if we made it, unbound, the pin
        // released after the destroy), never through ConsumeLocalActor, which would leave a rooted
        // PendingKill actor anchoring its world. Retire clears this drive.
        if (d.isTrashMirror) {
            coop::trash_mirror::Retire(d.lastEid, /*authoritative=*/false);
            ++released;
            continue;
        }
        // A prop goes back to physics and persists; a clump mirror is destroyed (transient, and its
        // holder's death-watch is gone). IsLiveByIndex: on the quit-to-menu path the world is dying
        // and a recycled slot passes plain IsLive, landing the physics call on a foreign occupant.
        if (R::IsLiveByIndex(d.actor, d.actorIdx)) {
            if (d.mesh) coop::prop_park::Unpark(d.actor, d.mesh, /*rootSimulates=*/true);
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
    // The leaver's generations must not gate the slot's next occupant, whose counter need not start
    // above them.
    g_holds[peerSlot] = {};
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
    // A held trash mirror retires whole, as in ForceRelease. Normally
    // trash_mirror::OnDisconnectForSlot already retired it (it runs first in DisconnectSlot) and
    // the drive is empty; Retire is idempotent.
    if (d.isTrashMirror) {
        coop::trash_mirror::Retire(d.lastEid, /*authoritative=*/false);  // clears this drive via ClearAnyDriveFor
        UE_LOGI("remote_prop: peer slot %d disconnected -- retired the held trash mirror eid=%u", peerSlot, d.lastEid);
        return;
    }
    if (d.mesh && R::IsLiveByIndex(d.actor, d.actorIdx)) {
        // A world prop goes back to physics and persists; by-index, for the recycled-slot hazard
        // above.
        coop::prop_park::Unpark(d.actor, d.mesh, /*rootSimulates=*/true);
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
