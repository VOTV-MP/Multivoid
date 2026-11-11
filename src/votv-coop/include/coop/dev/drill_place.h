// coop/dev/drill_place.h -- a drill's own placement of the local player at a fixed spot before a walk whose place the
// drill does not verify (the walk is a means: the drill judges the take, the throw, or the grab). The spots are where
// the drills' passing runs started from; see drill_place.cpp for why they are fixed.

#pragma once

#include <cstdint>

namespace ue_wrap { struct FVector; }

namespace coop::dev::drill_place {

struct Spot {
    float x, y, z;
    float yaw;
};

enum class Result { Pending, Landed, Failed };

// One placement, polled from a drill's tick: the first call teleports the local pawn to `spot`; later calls read it back.
// Landed once the pawn reads within kLandFlatCm of the spot and kLandUpCm vertically; Failed when the teleport call
// reports failure, the pawn does not read, or it has not landed within kLandBoundMs.
struct Placement {
    bool     issued = false;
    uint64_t atMs   = 0;
};

Result Run(Placement& p, void* pawn, const Spot& spot, uint64_t nowMs, const char* what);

}  // namespace coop::dev::drill_place
