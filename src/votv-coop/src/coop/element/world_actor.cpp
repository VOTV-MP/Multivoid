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
#include "ue_wrap/engine.h"
#include "ue_wrap/reflection.h"

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
    const ue_wrap::FVector tgtPos{snap.x, snap.y, snap.z};

    // First packet OR a teleport (error beyond the snap threshold) -> SNAP, no LERP across. WorldActors
    // have no streamed speed, so the snap threshold is a fixed distance (generous for a fast ship).
    const float dx = tgtPos.X - curPos_.X, dy = tgtPos.Y - curPos_.Y, dz = tgtPos.Z - curPos_.Z;
    const float distErr = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!hasPose_ || distErr > kSnapBaseCm) {
        curPos_ = tgtPos;
        curPitch_ = snap.pitch; curYaw_ = snap.yaw; curRoll_ = snap.roll;
        targetPos_ = tgtPos;
        targetPitch_ = snap.pitch; targetYaw_ = snap.yaw; targetRoll_ = snap.roll;
        errorPos_ = ue_wrap::FVector{};
        errorPitch_ = errorYaw_ = errorRoll_ = 0.f;
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
    errorPos_.X = tgtPos.X - curPos_.X;
    errorPos_.Y = tgtPos.Y - curPos_.Y;
    errorPos_.Z = tgtPos.Z - curPos_.Z;
    errorPitch_ = OffsetDegrees(curPitch_, snap.pitch);
    errorYaw_   = OffsetDegrees(curYaw_,   snap.yaw);
    errorRoll_  = OffsetDegrees(curRoll_,  snap.roll);
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
    dirty_ = true;
    if (arrived) {
        curPos_ = targetPos_;  // exact arrival (kills float drift over the window)
        curPitch_ = targetPitch_;
        curYaw_   = targetYaw_;
        curRoll_  = targetRoll_;
    }
}

void WorldActor::Tick() {
    void* actor = GetActor();
    if (!actor || !R::IsLiveByIndex(actor, GetInternalIdx())) return;  // unbound / GC'd mirror
    if (!hasPose_) return;  // no pose yet -- leave the mirror at its spawn transform
    AdvanceInterp();
    if (dirty_) { ApplyToEngine(); dirty_ = false; }
}

void WorldActor::ApplyToEngine() {
    void* actor = GetActor();
    if (!actor) return;
    // Transform-only drive with FULL rotation (a UFO/ship banks + rolls). No CMC (not an ACharacter)
    // and no mesh-offset reconstruction -- the mirror is the same class spawned at the same pivot.
    E::SetActorLocation(actor, curPos_);
    E::SetActorRotation(actor, ue_wrap::FRotator{curPitch_, curYaw_, curRoll_});
}

}  // namespace coop::element
