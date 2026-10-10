// ue_wrap/engine/engine_nav.h -- navigation and locomotion: a baked-NavMesh path query and pawn
// movement input, for the bot director. Engine-wrapper layer (principle 7): each call marshals one
// UFunction call or one reflected field access, with no gameplay, network or coop state. Game
// thread unless a declaration says otherwise. Implementation: src/ue_wrap/engine/engine_nav.cpp.

#pragma once

#include "ue_wrap/core/types.h"

#include <vector>

namespace ue_wrap::engine {

// A route from `start` to `end` (FindPathToLocationSynchronously on the CDO), its PathPoints into
// `outPts`; false on no route.
bool FindNavPath(void* worldContext, const FVector& start, const FVector& end,
                 std::vector<FVector>& outPts);

// APawn::AddMovementInput (resolved on Pawn): accumulates ControlInputVector, which the CMC
// consumes each tick, so re-issue it every frame.
void AddMovementInput(void* pawn, const FVector& worldDir, float scale, bool force);

// The limits a walking character moves under, from its CharacterMovement: the lowest floor normal
// Z it stands on (WalkableFloorZ) and the highest step it climbs. False when a read fails.
struct WalkLimits {
    float floorZ = 0.f;
    float stepCm = 0.f;
};
bool ReadWalkLimits(void* character, WalkLimits* out);

// The limits the level's NavMesh was built with (its RecastNavMesh): the steepest slope in degrees
// and the highest step a route may take, and the radius a route keeps from walls. False when none
// is loaded.
struct NavLimits {
    float maxSlopeDeg = 0.f;
    float stepCm = 0.f;
    float radiusCm = 0.f;
};
bool ReadNavLimits(NavLimits* out);

}  // namespace ue_wrap::engine
