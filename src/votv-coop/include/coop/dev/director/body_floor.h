// coop/dev/director/body_floor.h -- what a stuck walker's body stands on, said in the log. The NavMesh and the
// Static/Dynamic trace can both miss a support the body's own movement component stands on (a hatch, a slab, a
// barrier over a stairwell); this names it from three sides: the movement component's CurrentFloor, a down-trace over
// every object type a capsule blocks, and the actors by name near the walker. Dev director only; game thread; run
// once per run on the first persistent stuck, never per tick.

#pragma once

#include "coop/dev/director/director.h"   // PlayerContext

namespace coop::director {

// Logs `director/diag: stands on ...`, `director/diag: pawn-channel floor ...` and one `director/diag: near ...`
// per matching actor within 300 cm of the walker.
void SayWhatBodyStandsOn(const PlayerContext& ctx);

}  // namespace coop::director
