// coop/dev/level_ground.h -- TEST-ONLY: measured level open ground, for a drill that sets a machine down.
//
// A spot whose whole footprint is level, measured: nine line traces from above (the centre, the four corners and the four
// edge midpoints of a square 600 cm across) must all strike a floor, within 5 cm of one another in height, with the centre's
// surface normal within 3 degrees of vertical. A body that settles on such a square stands upright, whatever its base: a
// paper that merely rests where it fell proves nothing, since a paper rests on a 5 degree slope. Candidates are tried
// nearest first, 16 bearings to a ring, five rings from 30 m to 100 m; the first that passes, and that the caller accepts, is
// the answer.

#pragma once

#include "ue_wrap/core/types.h"

namespace coop::dev::level_ground {

// A caller's own test of a measured floor point (its footprint clear of other geometry): true takes it, false goes on to the
// next candidate. `walker` is the actor the traces ignore.
using Accept = bool (*)(void* ctx, void* walker, const ue_wrap::FVector& floorAt);

// Level open ground round `around`, tried nearest first. `at` is the floor at the square's centre. `walker` is the actor the
// traces ignore (a live player). Logs every refused candidate and the one taken. Game thread.
bool FindFrom(void* walker, const ue_wrap::FVector& around, ue_wrap::FVector& at, Accept accept = nullptr,
              void* ctx = nullptr);

}  // namespace coop::dev::level_ground
