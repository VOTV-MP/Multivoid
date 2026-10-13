// coop/player/hand_item_pose.cpp -- see coop/player/hand_item_pose.h.

#include "coop/player/hand_item_pose.h"

#include "coop/player/remote_player.h"
#include "ue_wrap/actors/puppet.h"  // GetSkeletalMeshComponent (owner head-bone anchor for the hold measure)
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>

namespace coop::hand_item_pose {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

constexpr float kHeadAnchorLiftCm = 33.f;  // the same lift as the puppet's head position

// View-space rotation math, in the engine's rotation-matrix conventions.
constexpr float kDeg2Rad  = 0.01745329252f;
constexpr float kRadToDeg = 57.2957795f;

// The engine's rotation matrix: rows are the rotation's world-space forward, right and up
// axes. A roll-0 call yields the view basis directly.
void RotMatrixRows(float pitchDeg, float yawDeg, float rollDeg, float M[3][3]) {
    const float SP = std::sin(pitchDeg * kDeg2Rad), CP = std::cos(pitchDeg * kDeg2Rad);
    const float SY = std::sin(yawDeg * kDeg2Rad),   CY = std::cos(yawDeg * kDeg2Rad);
    const float SR = std::sin(rollDeg * kDeg2Rad),  CR = std::cos(rollDeg * kDeg2Rad);
    M[0][0] = CP * CY;                   M[0][1] = CP * SY;                   M[0][2] = SP;
    M[1][0] = SR * SP * CY - CR * SY;    M[1][1] = SR * SP * SY + CR * CY;    M[1][2] = -SR * CP;
    M[2][0] = -(CR * SP * CY + SR * SY); M[2][1] = CY * SR - CR * SP * SY;    M[2][2] = CR * CP;
}

// The inverse: axis rows to pitch, yaw and roll.
ue_wrap::FRotator RotatorFromRows(const float M[3][3]) {
    ue_wrap::FRotator r{};
    r.Pitch = std::atan2(M[0][2], std::sqrt(M[0][0] * M[0][0] + M[0][1] * M[0][1])) * kRadToDeg;
    r.Yaw   = std::atan2(M[0][1], M[0][0]) * kRadToDeg;
    float S[3][3];
    RotMatrixRows(r.Pitch, r.Yaw, 0.f, S);  // roll-0 frame; roll = Z/Y vs its right axis
    const float zy = M[2][0] * S[1][0] + M[2][1] * S[1][1] + M[2][2] * S[1][2];
    const float yy = M[1][0] * S[1][0] + M[1][1] * S[1][1] + M[1][2] * S[1][2];
    r.Roll = std::atan2(zy, yy) * kRadToDeg;
    return r;
}

}  // namespace

// Measure the owner's natively welded held actor in its own view space. False means do not
// ship (the mesh or bone not readable yet, or a wild pre-weld transform); the periodic
// re-check retries.
bool MeasureLocalHoldRelative(void* local, void* held, float outPos[3], float outRot[3]) {
    void* mesh = ue_wrap::puppet::GetSkeletalMeshComponent(local);
    ue_wrap::FVector head{};
    if (!mesh || !R::IsLive(mesh) ||
        !E::GetBoneWorldLocationByName(mesh, L"head", head))
        return false;
    head.Z += kHeadAnchorLiftCm;
    void* ctl = E::GetController(local);
    ue_wrap::FRotator view{};
    if (ctl) view = E::GetControlRotation(ctl);
    else if (!E::TryGetActorRotation(local, view)) return false;
    float V[3][3];
    RotMatrixRows(ue_wrap::NormalizeAxis(view.Pitch), ue_wrap::NormalizeAxis(view.Yaw), 0.f, V);
    ue_wrap::FVector iloc{};
    if (!E::TryGetActorLocation(held, iloc)) return false;
    const float d[3] = {iloc.X - head.X, iloc.Y - head.Y, iloc.Z - head.Z};
    for (int i = 0; i < 3; ++i) {
        outPos[i] = d[0] * V[i][0] + d[1] * V[i][1] + d[2] * V[i][2];
        // A held item sits within arm's reach of the head; a wild read (the pre-weld spawn frame, a
        // dying component) must never ship.
        if (!(outPos[i] > -300.f && outPos[i] < 300.f)) return false;
    }
    float I[3][3], Q[3][3];
    ue_wrap::FRotator irot{};
    if (!E::TryGetActorRotation(held, irot)) return false;
    RotMatrixRows(irot.Pitch, irot.Yaw, irot.Roll, I);
    for (int r = 0; r < 3; ++r)      // item axis r expressed in the view frame
        for (int c = 0; c < 3; ++c)
            Q[r][c] = I[r][0] * V[c][0] + I[r][1] * V[c][1] + I[r][2] * V[c][2];
    const ue_wrap::FRotator rel = RotatorFromRows(Q);
    outRot[0] = rel.Pitch; outRot[1] = rel.Yaw; outRot[2] = rel.Roll;
    return true;
}

// Re-command the mirror to the puppet's view-space hold point every tick: rebuild the roll-0
// view basis from the synced aim and re-compose the owner-measured transform onto it. With
// the fallback this degrades to the camera-front placement, facing along the look.
void DriveMirror(void* mirrorActor, coop::RemotePlayer* pup, const float relPos[3], const float relRot[3]) {
    ue_wrap::FVector eye{};
    if (!pup->TryGetHeadPosition(eye)) return;   // no anchor this tick: the last placement stays
    const ue_wrap::FVector fwd = pup->GetSyncedAimDirection();
    // A degenerate near-vertical aim keeps the last tick's placement.
    const float rl = std::sqrt(fwd.X * fwd.X + fwd.Y * fwd.Y);
    if (rl < 1e-3f) return;
    const float yawDeg   = std::atan2(fwd.Y, fwd.X) * kRadToDeg;
    const float pitchDeg = std::atan2(fwd.Z, rl) * kRadToDeg;
    float V[3][3];
    RotMatrixRows(pitchDeg, yawDeg, 0.f, V);  // rows: fwd / right / up
    const float* rp = relPos;
    const ue_wrap::FVector pos{
        eye.X + V[0][0] * rp[0] + V[1][0] * rp[1] + V[2][0] * rp[2],
        eye.Y + V[0][1] * rp[0] + V[1][1] * rp[1] + V[2][1] * rp[2],
        eye.Z + V[0][2] * rp[0] + V[1][2] * rp[1] + V[2][2] * rp[2],
    };
    // The world item axes: row i of the composed matrix is the sum over c of Q[i][c] times V[c].
    float Q[3][3], W[3][3];
    RotMatrixRows(relRot[0], relRot[1], relRot[2], Q);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            W[i][j] = Q[i][0] * V[0][j] + Q[i][1] * V[1][j] + Q[i][2] * V[2][j];
    E::SetActorLocation(mirrorActor, pos);
    E::SetActorRotation(mirrorActor, RotatorFromRows(W));
}

}  // namespace coop::hand_item_pose
