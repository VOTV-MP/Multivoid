// coop/element/world_actor.cpp -- the CLIENT-mirror pose interpolator + engine drive for
// coop::element::WorldActor. See coop/element/world_actor.h. A SIBLING of npc_pose_drive.cpp
// (element::Npc) but transform-ONLY with FULL rotation: pos (vector error) + pitch/yaw/roll
// (each shortest-arc) and NO speed/CMC/kerfur (a WorldActor is a plain AActor -- UFO/ship/saucer
// -- not an ACharacter, so there is no CMC locomotion to drive).
//
// The interp is the proven advance-before-rebase shape (the interp-starvation fix,
// [[project-puppet-lag-interp-starvation]]): SetTargetPose advances the open window FIRST, then
// rebases. The drive writes the mirror's transform; the mirror's actor tick is parked at spawn
// (world_actor_sync: SetActorTickEnabled(false)) so this drive is authoritative -- no integration
// fight with the actor's own BP movement.

#include "coop/element/world_actor.h"

#include "coop/net/protocol.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cmath>
#include <cstdint>

namespace coop::element {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
// Shortest-arc delta in degrees, (-180, 180] -- avoids the 359->1 "long way round" spin (MTA;
// same helper as npc_pose_drive / remote_player).
float OffsetDegrees(float fromDeg, float toDeg) {
    float d = std::fmod(toDeg - fromDeg, 360.f);
    if (d > 180.f)  d -= 360.f;
    if (d < -180.f) d += 360.f;
    return d;
}

constexpr int   kInterpWindowMs = 75;     // same as RemotePlayer / Npc (the proven jitter window)
constexpr float kSnapBaseCm     = 2000.f; // > one window's legal motion for a fast UFO -> a real teleport, snap

}  // namespace

void WorldActor::SetTargetPose(const coop::net::WorldActorPoseSnapshot& snap) {
    // v102 aux target vec + v104 target identity: latest-wins, no interp (targets the mirror's
    // native easing / branch selection consume).
    auxX_ = snap.auxX; auxY_ = snap.auxY; auxZ_ = snap.auxZ;
    auxTargetEid_ = snap.auxTargetEid;
    const ue_wrap::FVector tgtPos{snap.x, snap.y, snap.z};

    // First packet OR a teleport (error beyond the snap threshold) -> SNAP, no LERP across. WorldActors
    // have no streamed speed, so the snap threshold is a fixed distance (generous for a fast ship).
    const float dx = tgtPos.X - curPos_.X, dy = tgtPos.Y - curPos_.Y, dz = tgtPos.Z - curPos_.Z;
    const float distErr = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!hasPose_ || distErr > kSnapBaseCm) {
        curPos_ = tgtPos;
        curPitch_ = snap.pitch; curYaw_ = snap.yaw; curRoll_ = snap.roll;
        curAuxYaw_ = snap.auxYaw;
        targetPos_ = tgtPos;
        targetPitch_ = snap.pitch; targetYaw_ = snap.yaw; targetRoll_ = snap.roll;
        targetAuxYaw_ = snap.auxYaw;
        errorPos_ = ue_wrap::FVector{};
        errorPitch_ = errorYaw_ = errorRoll_ = errorAuxYaw_ = 0.f;
        window_.Close();  // frozen at target (snapped)
        hasPose_ = true;
        dirty_ = true;
        return;
    }

    // Advance-before-rebase (load-bearing -- the interp-starvation fix): bring cur* to NOW using the
    // STILL-OPEN window's cached error BEFORE overwriting the target/error below.
    AdvanceInterp();

    targetPos_ = tgtPos;
    targetPitch_ = snap.pitch; targetYaw_ = snap.yaw; targetRoll_ = snap.roll;
    targetAuxYaw_ = snap.auxYaw;
    errorPos_.X = tgtPos.X - curPos_.X;
    errorPos_.Y = tgtPos.Y - curPos_.Y;
    errorPos_.Z = tgtPos.Z - curPos_.Z;
    errorPitch_ = OffsetDegrees(curPitch_, snap.pitch);
    errorYaw_   = OffsetDegrees(curYaw_,   snap.yaw);
    errorRoll_  = OffsetDegrees(curRoll_,  snap.roll);
    errorAuxYaw_ = OffsetDegrees(curAuxYaw_, snap.auxYaw);
    window_.Open(NowMs(), kInterpWindowMs);
    dirty_ = true;
}

void WorldActor::AdvanceInterp() {
    if (!window_.IsOpen()) return;  // no window open -- frozen at target
    bool arrived = false;
    const float dAlpha = window_.Advance(NowMs(), &arrived);  // shared alpha/dAlpha bookkeeping
    curPos_.X += errorPos_.X * dAlpha;
    curPos_.Y += errorPos_.Y * dAlpha;
    curPos_.Z += errorPos_.Z * dAlpha;
    curPitch_ += errorPitch_ * dAlpha;
    curYaw_   += errorYaw_   * dAlpha;
    curRoll_  += errorRoll_  * dAlpha;
    curAuxYaw_ += errorAuxYaw_ * dAlpha;
    dirty_ = true;
    if (arrived) {
        curPos_ = targetPos_;  // exact arrival (kills float drift over the window)
        curPitch_ = targetPitch_;
        curYaw_   = targetYaw_;
        curRoll_  = targetRoll_;
        curAuxYaw_ = targetAuxYaw_;
    }
}

void WorldActor::Tick() {
    // [WA-TRACE client-drive] 1 Hz per-mirror step/state trace (2026-07-05 0s-frozen-pyramid hunt).
    // `pre` = the engine location that SURVIVED the last frame (a restored BP tick that fights the
    // drive shows up as pre snapping back while cur/tgt advance); `post` = right after our write.
    const uint64_t nowMs = NowMs();
    const bool trace = (nowMs - dbgLastLogMs_ >= 1000);
    void* actor = GetActor();
    if (!actor || !R::IsLiveByIndex(actor, GetInternalIdx())) {
        if (trace) { dbgLastLogMs_ = nowMs;
            UE_LOGW("[WA-TRACE client-drive] eid=%u GUARD-FAIL actor=%p idx=%d liveByIdx=0 -- drive dead",
                    static_cast<uint32_t>(GetId()), actor, GetInternalIdx()); }
        return;  // unbound / GC'd mirror
    }
    if (!hasPose_) {
        if (trace) { dbgLastLogMs_ = nowMs;
            const auto pre = E::GetActorLocation(actor);
            UE_LOGI("[WA-TRACE client-drive] eid=%u NO-POSE-YET engine=(%.0f,%.0f,%.0f)",
                    static_cast<uint32_t>(GetId()), pre.X, pre.Y, pre.Z); }
        return;  // no pose yet -- leave the mirror at its spawn transform
    }
    ue_wrap::FVector pre{};
    if (trace) pre = E::GetActorLocation(actor);
    AdvanceInterp();
    if (dirty_) { ApplyToEngine(); dirty_ = false; }
    if (trace) {
        dbgLastLogMs_ = nowMs;
        const auto post = E::GetActorLocation(actor);
        UE_LOGI("[WA-TRACE client-drive] eid=%u pre=(%.0f,%.0f,%.0f) cur=(%.0f,%.0f,%.0f) "
                "tgt=(%.0f,%.0f,%.0f) post=(%.0f,%.0f,%.0f) window=%d applyLoc=%d applyRot=%d",
                static_cast<uint32_t>(GetId()), pre.X, pre.Y, pre.Z, curPos_.X, curPos_.Y, curPos_.Z,
                targetPos_.X, targetPos_.Y, targetPos_.Z, post.X, post.Y, post.Z,
                window_.IsOpen() ? 1 : 0, lastApplyLocOk_ ? 1 : 0, lastApplyRotOk_ ? 1 : 0);
    }
}

void WorldActor::ApplyToEngine() {
    void* actor = GetActor();
    if (!actor) return;
    // Transform-only drive with FULL rotation (a UFO/ship banks + rolls). No CMC (not an ACharacter)
    // and no mesh-offset reconstruction -- the mirror is the same class spawned at the same pivot.
    // Returns captured for [WA-TRACE client-drive]: K2_SetActorLocation/Rotation report failure via
    // ReturnValue and the drive must not stay blind to a silently-refused write.
    lastApplyLocOk_ = E::SetActorLocation(actor, curPos_);
    lastApplyRotOk_ = E::SetActorRotation(actor, ue_wrap::FRotator{curPitch_, curYaw_, curRoll_});
}

}  // namespace coop::element
