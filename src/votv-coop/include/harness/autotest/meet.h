// harness/autotest/meet.h -- TEST-ONLY: a two-peer drill's joiner reaches the host before the drill begins.
//
// A joiner appears beside the base bed, or at the KPP gate on days 0-2 (the join-day rule), and the host
// may stand elsewhere. A drill whose steps assume the two stand together would act from the wrong place.
// The step is the director's: a navmesh route walked through AddMovementInput, never a teleport onto a
// computed point, so the walker crosses ground a player could. The director stages every walk
// (coop/dev/director/stage.h): when the route to the host is long it puts the joiner at the known-good
// anchor nearest the host first, and VOTVCOOP_DRILL_PLACE=0 turns that off for a run that measures the walk.
//
// Called from the drill's own thread, not the game thread: it marshals its engine reads onto the game thread and
// blocks while the director walks.

#pragma once

#include "ue_wrap/core/types.h"

namespace harness::autotest::meet {

// Client only; true at once on the host. Walks the local player to within `reachCm` of the host's puppet. An
// unusable state logs `<tag>: VERDICT ABORT -- ...` and returns false: the drill ends there.
bool WalkToHost(const char* logTag, float reachCm = 300.f);

// Client only; true at once on the host. The same walk to a place in the world, `what` naming it in the log lines
// (the cargo lift drill's lift). The place is the caller's read of the target's own position; the stage and the
// route's budget are WalkToHost's.
bool WalkToPoint(const char* logTag, const ue_wrap::FVector& at, float reachCm, const char* what);

}  // namespace harness::autotest::meet
