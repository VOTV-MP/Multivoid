// coop/props/prop_drive_rules.h -- the send and rest rules of a driven prop, one rule for both authors.
//
// The host's driven rows (prop_drive_host) and a client that syncs a prop its interaction moves
// (prop_syncer_client) publish a pose when it moved past the same epsilons and end the drive when the
// body rested for the same time, so neither author carries a copy of the other's numbers.

#pragma once

#include "ue_wrap/core/types.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace coop::prop_drive_rules {

// A pose goes out when it moved past these since the last one that went out, so a creeping body
// still steps and a resting one costs nothing. One threshold on every axis, where MTA's
// unoccupied syncer (reference/mtasa-blue/Client/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp)
// holds x/y to FLOAT_EPSILON in WriteVehicleInformation and z to 0.1 -- 1.2 for a boat riding
// waves -- because z is where a parked vehicle is noisy; a prop on a floor has no such axis.
constexpr float kSendEpsCm  = 0.5f;
constexpr float kSendEpsDeg = 0.5f;
// A released prop that has not moved past the epsilons for this long has rested: the end edge.
constexpr uint64_t kRestMs = 500;
// A prop that has rested that long is read at 4 Hz, the hook lane's own resting cadence, instead
// of two dispatches per tick: an anchored tie holds a resting prop for as long as the hook stands.
constexpr uint64_t kRestProbeMs = 250;
// A velocity below this at the end goes out as zero: assigning a velocity wakes a body at rest.
constexpr float kRestVelCmS = 1.0f;
// A syncing client sends its prop's pose at most this often; the host sizes its late-syncer window by it. 25 ms
// is 40 Hz, the ATV's kStreamBudget refill, so the host's burst of 16 absorbs the jitter of a send at this rate.
// MTA's default is a row every 400 ms carrying the move and turn speed (CMainConfig.cpp:1513); ours carry the pose and the
// velocity at 40 Hz because the receiver lerps to the pose and the velocity serves the host's takeover of a thrown prop.
constexpr uint64_t kClientDriveSendMs = 25;
// How many props may coast with no verb's claim: one bound for a born prop's fall, a hand's let-go and a body's push, so
// the per-tick reads of the coasting rows stay bounded. A coast asked past it is refused and counted
// (prop_drive_host::CoastCapReached).
constexpr size_t kMaxCoasting = 32;
// The last rows of that bound a born prop's fall may not take: a let-go and a body's push still coast when a pile of births
// (a spawn-menu pile, an event's debris) stands at the births' own bound, kMaxCoasting less this.
constexpr size_t kCoastPushReserve = 8;

// The angle in degrees of the turn from `b` to `a`: the quaternions' (UE4.27's FRotator::Quaternion) angle, not the axes'
// differences. A prop lying at pitch -90 has one pose for every yaw and roll of the same sum (at +90, of the same
// difference); the game reads it back with a different pair from one read to the next, so a still drive on its side
// moves 6-90 degrees on each axis.
inline float TurnDeg(const ue_wrap::FRotator& a, const ue_wrap::FRotator& b) {
    constexpr double kHalfRad = 3.14159265358979323846 / 360.0;
    auto quat = [](const ue_wrap::FRotator& r, double q[4]) {
        const double sp = std::sin(r.Pitch * kHalfRad), cp = std::cos(r.Pitch * kHalfRad);
        const double sy = std::sin(r.Yaw * kHalfRad), cy = std::cos(r.Yaw * kHalfRad);
        const double sr = std::sin(r.Roll * kHalfRad), cr = std::cos(r.Roll * kHalfRad);
        q[0] = cr * sp * sy - sr * cp * cy;
        q[1] = -cr * sp * cy - sr * cp * sy;
        q[2] = cr * cp * sy - sr * sp * cy;
        q[3] = cr * cp * cy + sr * sp * sy;
    };
    double qa[4], qb[4];
    quat(a, qa);
    quat(b, qb);
    const double dot = std::fabs(qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3]);
    return static_cast<float>(std::acos(dot < 1.0 ? dot : 1.0) / kHalfRad);
}

// The boot selftest of TurnDeg and MovedPastEps: a still prop on its side is no turn, a real turn reads as its angle.
bool RunTurnDegSelftest();

// True when a pose moved past the send epsilons since `sentLoc`/`sentRot`, the last one that went out: any axis of the
// location, or the turn's angle.
inline bool MovedPastEps(const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot,
                         const ue_wrap::FVector& sentLoc, const ue_wrap::FRotator& sentRot) {
    if (std::fabs(loc.X - sentLoc.X) > kSendEpsCm) return true;
    if (std::fabs(loc.Y - sentLoc.Y) > kSendEpsCm) return true;
    if (std::fabs(loc.Z - sentLoc.Z) > kSendEpsCm) return true;
    return TurnDeg(rot, sentRot) > kSendEpsDeg;
}

}  // namespace coop::prop_drive_rules
