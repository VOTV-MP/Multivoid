// harness/autotest/meet.h -- TEST-ONLY: a two-peer drill's joiner reaches the host before the drill begins.
//
// A joiner appears beside the base bed, or at the KPP gate on days 0-2 (the join-day rule), and the host
// may stand elsewhere. A drill whose steps assume the two stand together would act from the wrong place.
// The default step is the director's: a navmesh route walked through AddMovementInput, never a teleport,
// so the walker crosses ground a player could.
//
// PlaceBesideHost is the additional measure for a drill that only needs the two players together: it
// teleports the joiner to the known-good anchor nearest the host (the base bed spot, the KPP gate -- spots
// the out-of-bounds check has already accepted) through the game's own teleportWObackrooms, then walks the
// short rest with the director. A computed offset is never teleported onto: it can land inside geometry.
// VOTVCOOP_MEET_PLACE=1 routes every WalkToHost through it, for a run that only wants its time back.
//
// Called from the drill's own thread, not the game thread: it marshals its engine reads onto the game thread and
// blocks while the director walks.

#pragma once

namespace harness::autotest::meet {

// Client only; true at once on the host. Walks the local player to within `reachCm` of the host's puppet. An
// unusable state logs `<tag>: VERDICT ABORT -- ...` and returns false: the drill ends there. With
// VOTVCOOP_MEET_PLACE=1 it is PlaceBesideHost.
bool WalkToHost(const char* logTag, float reachCm = 300.f);

// Client only; true at once on the host. Teleports the local player to the anchor nearest the host's puppet
// (skipped when the player is already nearer the host than that anchor), waits on the pawn's location, then
// walks the rest as WalkToHost does. Same ABORT contract.
bool PlaceBesideHost(const char* logTag, float reachCm = 300.f);

}  // namespace harness::autotest::meet
