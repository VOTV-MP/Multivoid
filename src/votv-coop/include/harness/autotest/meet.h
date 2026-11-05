// harness/autotest/meet.h -- TEST-ONLY: a two-peer drill's joiner walks to the host before the drill begins.
//
// On the rig save the host spawns at the base and the joining client at the far default spawn, hundreds of metres
// away (measured: (-2020,264) against (-37640,70005), 356 m). A drill whose steps assume the two stand together
// would act from the far spawn. The step is the director's: a navmesh route walked through AddMovementInput, never a
// teleport, so the walker crosses ground a player could.
//
// Called from the drill's own thread, not the game thread: it marshals its engine reads onto the game thread and
// blocks while the director walks.

#pragma once

namespace harness::autotest::meet {

// Client only; true at once on the host. Walks the local player to within `reachCm` of the host's puppet. An
// unusable state logs `<tag>: VERDICT ABORT -- ...` and returns false: the drill ends there.
bool WalkToHost(const char* logTag, float reachCm = 300.f);

}  // namespace harness::autotest::meet
