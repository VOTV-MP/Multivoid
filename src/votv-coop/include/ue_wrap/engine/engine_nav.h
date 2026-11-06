// ue_wrap/engine/engine_nav.h -- navigation and locomotion: a baked-NavMesh path query and pawn
// movement input, for the bot director. Engine-wrapper layer (principle 7): each call marshals one
// UFunction call or one reflected field access, with no gameplay, network or coop state. Game
// thread unless a declaration says otherwise. Implementation: src/ue_wrap/engine/engine_nav.cpp.

#pragma once

#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/hit_result.h"

#include <vector>

namespace ue_wrap::engine {

// A route from `start` to `end` (FindPathToLocationSynchronously on the CDO), its PathPoints into
// `outPts`; false on no route.
bool FindNavPath(void* worldContext, const FVector& start, const FVector& end,
                 std::vector<FVector>& outPts);

// The NavMesh point nearest `point` within `extent` (K2_ProjectPointToNavigation on the CDO, the
// default NavMesh, no filter); false when none lies within it.
bool ProjectToNav(void* worldContext, const FVector& point, const FVector& extent, FVector* out);

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

// What a walking character's movement component says it stands on: its CurrentFloor (FFindFloorResult), the floor
// its last walking move found, by the struct's reflected member names. `hit` is the floor's FHitResult (actor,
// component, impact point and normal). False, with the missing step logged, when the character has no movement
// component, the property or the struct's members do not resolve, or the hit does not read.
struct FloorRead {
    bool  blocking = false;     // bBlockingHit: the floor sweep hit something
    bool  walkable = false;     // bWalkableFloor: what it hit is a floor the body may stand on
    float floorDist = 0.f;      // FloorDist: capsule bottom to the floor, cm
    hit_result::Hit hit;
};
bool ReadCurrentFloor(void* character, FloorRead* out);

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
