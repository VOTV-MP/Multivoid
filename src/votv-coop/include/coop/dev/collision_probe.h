// coop/dev/collision_probe.h -- TEST-ONLY probe (ini collision_probe=true / env VOTVCOOP_COLLISION_PROBE; default off): what
// collides with what on a pawn, as the engine reports it at run time.
//
// The Blueprint dumps and the live run disagree about whether a pawn's capsule blocks the props' channel, so the answer is read
// from the engine: every primitive component of a pawn with its collision-enabled flag, object type and responses to the
// channels a prop meets, logged when the pawn is first seen (a puppet at its spawn, the local pawn at its first tick and again at
// its world-ready) and again two seconds later, after the Blueprint's own runtime collision writes have had their chance. The
// body-push drill adds the woodchipper's root at its spawn and, per push, each pawn component against the prop's root: the
// responses each has to the other's type, whether that pair blocks, the engine's own overlap flag and the distance. Lines:
//   [COLLISION-PROBE] <label> <class>@<ptr> comp=<name> class=<class> enabled=<0-3> type=<channel> profile=<name> PB=<i|o|b> ...
//   [COLLISION-PROBE] <label> pair pawn comp=<name> (<type>) vs prop <name> (<type>): ... mutual=<ignore|overlap|block> ...
// No verdict reads it. Game thread.

#pragma once

namespace coop::dev::collision_probe {

// True when the row is set.
bool IsEnabled();

// Every tick in a world: logs the local pawn and each puppet the first time it is seen, and two seconds later.
void Tick();

// Every primitive component of `actor`, one line each, under `label`. Nothing when the row is off.
void LogActor(const char* label, void* actor);

// Each primitive component of `pawn` against `propRoot`, one line each, under `label`. Nothing when the row is off.
void LogPair(const char* label, void* pawn, void* propRoot);

}  // namespace coop::dev::collision_probe
