// coop/player/remote_player_ragdoll.cpp -- see coop/player/remote_player_ragdoll.h.

#include "coop/player/remote_player_ragdoll.h"

#include "coop/dev/ragdoll_master_pose.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

namespace coop {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

bool RagdollDisplay::OnWireBit(bool ragdollBit, void* puppetActor, int32_t puppetIdx) {
    if (ragdollBit == wireState_) return false;
    wireState_ = ragdollBit;
    if (ragdollBit) {
        Start(puppetActor, puppetIdx);
        return false;
    }
    Stop(puppetActor, puppetIdx);
    return true;
}

bool RagdollDisplay::SetPose(const coop::net::RagdollPoseSnapshot& snap) {
    lastPose_ = snap;
    hasPose_ = true;
    if (active_ && body_ && R::IsLiveByIndex(body_, bodyIdx_)) {
        E::DriveRagdollBodyPelvisVelocity(
            body_,
            ue_wrap::FVector{snap.linVelX, snap.linVelY, snap.linVelZ},
            ue_wrap::FVector{snap.angVelX, snap.angVelY, snap.angVelZ});
        return true;
    }
    return false;
}

RagdollDisplay::Drive RagdollDisplay::DriveAttached(void* puppetActor, int32_t puppetIdx) {
    if (!active_) return Drive::Inactive;
    if (body_ && R::IsLiveByIndex(body_, bodyIdx_)) {
        // The pelvis attach follows the body's POSITION, but the puppet is a
        // character so its capsule stays UPRIGHT (kel slides + yaws but never
        // tumbles). Drive the pelvis WORLD rotation onto the puppet each frame
        // so the Dr. Kel skin tumbles WITH the flop too. v22: once the peer's
        // ragdoll PHYSICS stream is flowing, use the EXACT streamed pelvis
        // rotation (the mirror body's velocity is slaved to the sender, but
        // reading the streamed rotation directly removes any dependence on the
        // local sim matching -- the kel orientation is then identical to the
        // sender's real ragdoll). Before the first packet, bootstrap from the
        // mirror body's own pelvis.
        ue_wrap::FRotator pr;
        if (hasPose_) {
            pr = ue_wrap::FRotator{lastPose_.pitch, lastPose_.yaw, lastPose_.roll};
            E::SetActorRotation(puppetActor, pr);
        } else if (E::GetRagdollBodyPelvisRotation(body_, pr)) {
            E::SetActorRotation(puppetActor, pr);
        }
        // Master-pose PROBE (dev, OFF by default -- coop/dev/ragdoll_master_pose.h):
        // couple the remaining 5 bones too. Manages its own apply/pin/restore off its
        // enable state; inert (atomic load + map lookup) while disabled.
        dev::ragdoll_master_pose::Drive(puppetActor, puppetIdx, body_, bodyIdx_);
        return Drive::Attached;
    }
    // Self-heal (audit 2026-06-01): the body was GC-killed mid-ragdoll (e.g. a
    // level transition reaped it) -- recover NOW (detach + clear) so the puppet
    // isn't stuck attached to a dead component; the owner resumes pose-drive
    // this same tick. See [[project-ragdoll-sync]].
    Stop(puppetActor, puppetIdx);
    return Drive::StoppedNow;
}

void RagdollDisplay::TeardownForDestroy() {
    // The body is a SEPARATE actor that would otherwise outlive the puppet as
    // an orphan. IsLiveByIndex-guarded so a GC-recycled address isn't mistaken
    // for our body. No detach: the puppet actor is destroyed right after.
    if (body_ && R::IsLiveByIndex(body_, bodyIdx_)) E::DestroyActor(body_);
    body_ = nullptr;
    bodyIdx_ = -1;
    wireState_ = false;  // a recycled puppet starts un-ragdolled + re-converges
    active_ = false;
    hasPose_ = false;    // v22: drop stale streamed ragdoll pelvis state
}

void RagdollDisplay::Start(void* puppetActor, int32_t puppetIdx) {
    if (!puppetActor || !R::IsLiveByIndex(puppetActor, puppetIdx)) return;

    // Spawn an INVISIBLE playerRagdoll_C physics body co-located with the puppet
    // (its own "plushy" mesh is hidden inside SpawnPlayerRagdollBody). If it
    // fails, leave the puppet UPRIGHT + pose-driving (active_ stays false) --
    // graceful. The body's Player @0x248 = this puppet; the spawn is DEATH-FREE
    // (no ragdollMode).
    const ue_wrap::FVector loc = E::GetActorLocation(puppetActor);
    const ue_wrap::FRotator rot = E::GetActorRotation(puppetActor);
    void* body = E::SpawnPlayerRagdollBody(puppetActor, loc, rot);
    if (!body) {
        UE_LOGW("RagdollDisplay::Start: playerRagdoll_C spawn failed -- puppet stays upright (graceful)");
        return;
    }
    body_ = body;
    bodyIdx_ = R::InternalIndexOf(body);

    // PELVIS-attach the VISIBLE Dr. Kel puppet to the invisible ragdoll body so
    // it tumbles WITH the physics (the user's "pelvis to pelvis attachment").
    // The kel skin stays visible -- it IS the funny ragdoll visual. No skeleton
    // binding needed (rigid transform follow). If the attach fails, destroy the
    // body + stay upright (graceful).
    if (!E::AttachActorToRagdollBody(puppetActor, body)) {
        UE_LOGW("RagdollDisplay::Start: pelvis-attach failed -- destroying body, puppet stays upright");
        E::DestroyActor(body);
        body_ = nullptr; bodyIdx_ = -1;
        return;
    }
    active_ = true;
    UE_LOGI("RagdollDisplay::Start: spawned invisible ragdoll body=%p, pelvis-attached the puppet -- it tumbles along", body);
}

void RagdollDisplay::Stop(void* puppetActor, int32_t puppetIdx) {
    // Un-slave the kel meshes + restore their attach-relative transforms BEFORE the
    // master body is detached/destroyed (no-op when the probe never applied).
    dev::ragdoll_master_pose::Stop(puppetActor);
    // Detach the puppet (KeepWorld -- it stays where the flop left it; the next
    // pose drives it back to the streamed pose) BEFORE destroying the body it's
    // attached to.
    if (puppetActor && R::IsLiveByIndex(puppetActor, puppetIdx)) E::DetachActorFromRagdollBody(puppetActor);
    // Destroy the invisible physics body (IsLiveByIndex-guarded against a recycled addr).
    if (body_ && R::IsLiveByIndex(body_, bodyIdx_)) E::DestroyActor(body_);
    body_ = nullptr;
    bodyIdx_ = -1;
    active_ = false;
    hasPose_ = false;  // v22: next ragdoll bootstraps rotation fresh (not stale streamed)
    UE_LOGI("RagdollDisplay::Stop: detached puppet + destroyed ragdoll body");
}

}  // namespace coop
