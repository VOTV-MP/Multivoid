// coop/props/constraint_ties.h -- which actors a live physics constraint ties together, learned from the engine's own
// constraint calls rather than read off the constraints.
//
// A Blueprint reaches UPhysicsConstraintComponent::SetConstrainedComponents (a nail, a cord, a wire, a hook) by a final
// call into a native, which the ProcessEvent detour never sees, so the tie is caught at the native seam: a PRE hook reads
// the call's two components, and the owner actor of each is what the constraint ties. BreakConstraint's POST hook ends the
// tie. Both patches go on at boot, before any world loads, so the ties a save restores are seen as they are built. A
// constraint configured by name in the level (ConstraintActor1/2, no call) is read once per world by a walk of the live
// constraint components, at the first question after the world changed.
//
// The index is world state, never session state: no session clears it. A tie whose constraint or owner no longer lives is
// dropped when read. The prop drive's join (coop/props/prop_drive_join) asks it which props a driven prop drags.
// MTA ties a trailer by a link between two synced elements (reference/mtasa-blue/Server/mods/deathmatch/logic/
// CVehicle.cpp:713, SetTowedVehicle); here the engine's own constraint call is the event that makes the link.
// Game thread, all of it.

#pragma once

#include <cstddef>

namespace coop::constraint_ties {

// Put both native patches on: SetConstrainedComponents (pre) and BreakConstraint (post). Idempotent and cheap once live;
// harness::Start posts it as a first game-thread task and the frame tail calls it again until both are on, since the class
// and the script gate's exec-handler table may not be ready at the first try. Says `constraint_ties: seams live` once.
void Install();

// The actors a live constraint ties `actor` to, written to `out` (at most `cap`, each once per constraint, so an actor
// tied by two constraints appears twice). The first call after the world changed reads the by-name constraints of the new
// world and drops the old world's entries. Returns how many were written.
size_t TiesOf(void* actor, void** out, size_t cap);

// How many constraint entries the index holds now (stale ones included until a read drops them).
size_t Count();

// A function told, from inside BreakConstraint's post patch, which constraint component was broken: the index has dropped
// its entry already. It runs deep in an engine call, so it must call no hooked native. Two slots; a second add of the same function
// is a no-op; false when full.
using BreakFn = void (*)(void* constraint);
bool AddBreakListener(BreakFn fn);

// BreakConstraint's post patch is on: the witness a drill's break listener waits for.
bool BreakSeamLive();

}  // namespace coop::constraint_ties
