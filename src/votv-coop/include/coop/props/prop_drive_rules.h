// coop/props/prop_drive_rules.h -- the send and rest rules of a driven prop, one rule for both authors.
//
// The host's driven rows (prop_drive_host) and a client that syncs a prop its interaction moves
// (prop_syncer_client) publish a pose when it moved past the same epsilons and end the drive when the
// body rested for the same time, so neither author carries a copy of the other's numbers.

#pragma once

#include "ue_wrap/core/types.h"

#include <cmath>
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
// MTA's default is a row every 400 ms carrying the move and turn speed (CMainConfig.cpp:1513); ours are pose-only
// at 40 Hz because the receiver only lerps.
constexpr uint64_t kClientDriveSendMs = 25;

// True when a pose moved past the send epsilons on any axis since `sentLoc`/`sentRot`, the last one that went out.
inline bool MovedPastEps(const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot,
                         const ue_wrap::FVector& sentLoc, const ue_wrap::FRotator& sentRot) {
    if (std::fabs(loc.X - sentLoc.X) > kSendEpsCm) return true;
    if (std::fabs(loc.Y - sentLoc.Y) > kSendEpsCm) return true;
    if (std::fabs(loc.Z - sentLoc.Z) > kSendEpsCm) return true;
    if (std::fabs(ue_wrap::NormalizeAxis(rot.Pitch - sentRot.Pitch)) > kSendEpsDeg) return true;
    if (std::fabs(ue_wrap::NormalizeAxis(rot.Yaw   - sentRot.Yaw))   > kSendEpsDeg) return true;
    if (std::fabs(ue_wrap::NormalizeAxis(rot.Roll  - sentRot.Roll))  > kSendEpsDeg) return true;
    return false;
}

}  // namespace coop::prop_drive_rules
