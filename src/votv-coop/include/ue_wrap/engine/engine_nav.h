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

}  // namespace ue_wrap::engine
