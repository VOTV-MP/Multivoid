// coop/npc_pose_drive.cpp -- the CLIENT-mirror pose interpolator + engine drive for
// coop::element::Npc. See coop/element/npc.h. A SUBSET of remote_player.cpp (pos + yaw +
// speed + stateBits; no pitch/headYawDelta/vitals/ragdoll/mesh-offset).
//
// The interp is the proven advance-before-rebase shape (the interp-starvation fix,
// [[project-puppet-lag-interp-starvation]]): SetTargetNpcPose advances the open window FIRST,
// then rebases. The drive writes the mirror's transform + CMC.Velocity/MovementMode so the
// NPC's OWN AnimBP animates natively (NPCs are ACharacter subclasses with the same CMC layout
// as the player puppet). The mirror's CMC tick is parked at spawn (npc_mirror) so this drive
// is authoritative -- no integration fight.

#include "coop/element/npc.h"

#include "coop/net/protocol.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/kerfur.h"   // v74: DriveKerfurState (host-authoritative command/spooky on the mirror)
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/wisp.h"     // 2026-07-03: DriveWispLanding (fade-in edge on the wisp_C mirror)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace coop::element {
namespace {

namespace E   = ue_wrap::engine;
namespace Pup = ue_wrap::puppet;
namespace R   = ue_wrap::reflection;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
// Shortest-arc delta in degrees, (-180, 180] -- avoids the 359->1 "long way round" spin (MTA).
float OffsetDegrees(float fromDeg, float toDeg) {
    float d = std::fmod(toDeg - fromDeg, 360.f);
    if (d > 180.f)  d -= 360.f;
    if (d < -180.f) d += 360.f;
    return d;
}

constexpr int   kInterpWindowMs  = 75;     // same as RemotePlayer (the proven jitter window)
constexpr float kSnapBaseCm      = 1000.f; // > one window's legal motion -> a real teleport, snap
constexpr float kSnapPerSpeedSec = 0.5f;

}  // namespace

void Npc::SetTargetNpcPose(const coop::net::EntityPoseSnapshot& snap) {
    const ue_wrap::FVector tgtPos{snap.x, snap.y, snap.z};

    // v39 head-look: store the streamed lookAt (a WORLD target, independent of the pos interp --
    // the native FAnimNode_LookAt node smooths it via its own InterpSpeed, so no LERP here).
    // Refreshed on EVERY packet (incl. a head-only turn with a stationary body), so the mirror
    // tracks the host kerfur's gaze at ~sendHz. Cleared when the host stops sending it.
    hasLookAt_ = (snap.stateBits & coop::net::kEntityPoseBitHasLookAt) != 0;
    if (hasLookAt_) curLookAt_ = ue_wrap::FVector{snap.lookAtX, snap.lookAtY, snap.lookAtZ};

    // v40 body-facing: the visible-body (ACharacter::Mesh) world yaw, interpolated like the actor
    // yaw (shortest-arc) and driven onto the mirror mesh in ApplyToEngine. Cleared -> body interp
    // term goes to a no-op when the host stops sending it (non-kerfur NPCs never set the bit).
    hasBodyYaw_ = (snap.stateBits & coop::net::kEntityPoseBitHasBodyYaw) != 0;
    if (!hasBodyYaw_) errorBodyYaw_ = 0.f;

    // v74 host-authoritative kerfur command/spooky (snapped, not interpolated -- it selects the
    // AnimBP state, not a pose). Applied in ApplyToEngine onto the parked mirror (which runs no AI
    // so nothing fights the write). Non-kerfur NPCs never set the bit -> hasKerfState_ stays false.
    hasKerfState_ = (snap.stateBits & coop::net::kEntityPoseBitHasKerfurState) != 0;
    if (hasKerfState_) {
        kerfState_  = snap.kerfState;
        kerfSpooky_ = (snap.stateBits & coop::net::kEntityPoseBitKerfurSpooky) != 0;
    }

    // First packet OR a teleport (error beyond the snap threshold) -> SNAP, no LERP across.
    const float dx = tgtPos.X - curPos_.X, dy = tgtPos.Y - curPos_.Y, dz = tgtPos.Z - curPos_.Z;
    const float distErr = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float snapThresh = kSnapBaseCm + kSnapPerSpeedSec * snap.speed;
    if (!hasPose_ || distErr > snapThresh) {
        curPos_ = tgtPos;
        curYaw_ = snap.yaw;
        curSpeed_ = snap.speed;
        curStateBits_ = snap.stateBits;
        targetPos_ = tgtPos;
        targetYaw_ = snap.yaw;
        errorPos_ = ue_wrap::FVector{};
        errorYaw_ = 0.f;
        if (hasBodyYaw_) { curBodyYaw_ = snap.bodyYaw; targetBodyYaw_ = snap.bodyYaw; errorBodyYaw_ = 0.f; }
        window_.Close();  // frozen at target (snapped)
        hasPose_ = true;
        dirty_ = true;
        return;
    }

    // Advance-before-rebase (load-bearing -- the interp-starvation fix): bring curPos_ to NOW
    // using the STILL-OPEN window's cached error BEFORE overwriting the target/error below.
    AdvanceInterp();

    targetPos_ = tgtPos;
    targetYaw_ = snap.yaw;
    curSpeed_ = snap.speed;          // not interpolated -- the AnimBP blends locomotion
    curStateBits_ = snap.stateBits;  // snapped
    errorPos_.X = tgtPos.X - curPos_.X;
    errorPos_.Y = tgtPos.Y - curPos_.Y;
    errorPos_.Z = tgtPos.Z - curPos_.Z;
    errorYaw_ = OffsetDegrees(curYaw_, snap.yaw);
    if (hasBodyYaw_) { targetBodyYaw_ = snap.bodyYaw; errorBodyYaw_ = OffsetDegrees(curBodyYaw_, snap.bodyYaw); }
    window_.Open(NowMs(), kInterpWindowMs);
    dirty_ = true;
}

void Npc::AdvanceInterp() {
    if (!window_.IsOpen()) return;  // no window open -- frozen at target
    bool arrived = false;
    const float dAlpha = window_.Advance(NowMs(), &arrived);  // shared alpha/dAlpha bookkeeping
    curPos_.X += errorPos_.X * dAlpha;
    curPos_.Y += errorPos_.Y * dAlpha;
    curPos_.Z += errorPos_.Z * dAlpha;
    curYaw_     += errorYaw_     * dAlpha;
    curBodyYaw_ += errorBodyYaw_ * dAlpha;  // v40 (no-op when errorBodyYaw_==0 i.e. not tracking)
    dirty_ = true;
    if (arrived) {
        curPos_ = targetPos_;  // exact arrival (kills float drift over the window)
        curYaw_ = targetYaw_;
        curBodyYaw_ = targetBodyYaw_;
    }
}

void Npc::Tick() {
    void* actor = GetActor();
    if (!actor || !R::IsLiveByIndex(actor, GetInternalIdx())) return;  // unbound / GC'd mirror
    if (!hasPose_) return;  // no pose yet -- leave the mirror at its spawn transform
    AdvanceInterp();
    if (dirty_) { ApplyToEngine(); dirty_ = false; }
}

void Npc::ApplyToEngine() {
    void* actor = GetActor();
    if (!actor) return;
    // NPC capsule centre IS the actor pivot (the host streams GetActorLocation, the mirror is the
    // same class spawned at the same place) -- no mesh-offset reconstruction (unlike RemotePlayer).
    E::SetActorLocation(actor, curPos_);
    E::SetActorRotation(actor, ue_wrap::FRotator{0.f, curYaw_, 0.f});
    // Drive the mirror's OWN CMC so its AnimBP reads the right Velocity + MovementMode (the native
    // locomotion path -- the same fields the host NPC's possessed CMC carries). Reconstruct planar
    // velocity from the streamed body-yaw + speed magnitude.
    const float yawRad = curYaw_ * 0.01745329252f;  // PI/180
    const ue_wrap::FVector vel{ std::cos(yawRad) * curSpeed_, std::sin(yawRad) * curSpeed_, 0.f };
    const bool inAir = (curStateBits_ & coop::net::kStateBitInAir) != 0;
    Pup::DriveCharacterMovement(actor, vel, inAir);
    // 2026-07-03 wisp mirror: replay the native landing edge (landed=true + dir(true) -> the
    // fade-in the wisp gates behind a CMC-tick-computed CurrentFloor read its parked CMC can
    // never produce). Grounded-per-host = drive; retried per frame until wisp_C resolves (also
    // covers a joiner mirroring an already-landed wisp: its first pose reads grounded).
    if (isWispMirror_ && !wispLanded_ && !inAir)
        wispLanded_ = ue_wrap::wisp::DriveWispLanding(actor);
    // v39: aim the mirror's head/neck at the host's streamed look target. Writes the kerfur
    // AnimBP `lookAt` + sets customLookAt=true so the mirror's own BUA stops auto-aiming at the
    // LOCAL player camera (the desync this fixes). Class-gated inside -> safe no-op on non-kerfur
    // NPCs. Re-asserted each drive so a moving gaze tracks (customLookAt persists once set).
    if (hasLookAt_) Pup::DriveKerfurLookAt(actor, curLookAt_);
    // v40: drive the VISIBLE body facing -- set the mirror's ACharacter::Mesh WORLD yaw to the
    // host's streamed (interpolated) value. MUST be AFTER SetActorRotation above (moving the actor
    // root re-bases this child mesh's world transform). The mirror's actor tick is off, so the
    // kerfur BP never overwrites it -> no gate flag needed. Class-gated inside -> safe on non-kerfur.
    if (hasBodyYaw_) Pup::DriveKerfurBodyYaw(actor, curBodyYaw_);
    // v74: write the host-authoritative kerfur command + spooky flag so the parked mirror's AnimBP
    // state machine matches the host (the mirror runs no AI -> it can't pick its own). Class-gated
    // inside -> safe no-op on non-kerfur NPCs.
    if (hasKerfState_) ue_wrap::kerfur::DriveKerfurState(actor, kerfState_, kerfSpooky_);
}

}  // namespace coop::element
