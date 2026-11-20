// coop/props/constraint_ties.h -- which actors a live physics constraint ties together, learned from the engine's own
// constraint calls rather than read off the constraints.
//
// A Blueprint reaches UPhysicsConstraintComponent::SetConstrainedComponents (a nail, a cord, a wire, a hook) by a final
// call into a native, which the ProcessEvent detour never sees, so the tie is caught at the native seam: a PRE hook reads
// the call's two components, and the owner actor of each is what the constraint ties. BreakConstraint's POST hook ends the
// tie. Both patches are tried at boot, before any world loads, so the ties a save restores are seen as they are built; a
// host session that starts with one missing says so in the log. A constraint configured by name in the level
// (ConstraintActor1/2, no call) is read once per world, at the first question after the world changed.
//
// The index is world state, never session state: no session clears it. A tie whose constraint or owner no longer lives is
// dropped when read. The prop drive's join (coop/props/prop_drive_join) asks it which props a driven prop drags.
// MTA's client reads a trailer link off the game each sync pulse (reference/mtasa-blue/Client/mods/deathmatch/logic/
// CUnoccupiedVehicleSync.cpp:289-291, 359-363); here the engine's own constraint call is the event that makes the link.
// Game thread, all of it.

#pragma once

#include <cstddef>

namespace coop::constraint_ties {

// Put both native patches on: SetConstrainedComponents (pre) and BreakConstraint (post). Idempotent and cheap once live;
// harness::Start posts it as a first game-thread task and the frame tail calls it again until both are on, since the class
// and the script gate's exec-handler table may not be ready at the first try. Says `constraint_ties: seams live` once.
void Install();

// The actors a live constraint ties `actor` to, written to `out` (at most `cap`, each partner once, however many
// constraints join the two). A prop with more partners than `cap` says so in the log. The first call after the world
// changed reads the by-name constraints of the new world and drops the old world's entries. Returns how many were written.
size_t TiesOf(void* actor, void** out, size_t cap);

// How many constraint entries the index holds now (stale ones included until a read drops them).
size_t Count();

// HOST: the live owner of each tie made since the last call is handed to `fn`, then the list is emptied. The patch only notes
// the owners, so what is asked of them (driven? held?) is asked here, on the frame call, never inside the engine's call.
using TiedFn = void (*)(void* owner);
void DrainTied(TiedFn fn);

// A function told, from inside BreakConstraint's post patch, which constraint component was broken: the index has dropped
// its entry already. It runs deep in an engine call, so it must call no hooked native. Two slots; a second add of the same function
// is a no-op; false when full (the log says so once).
using BreakFn = void (*)(void* constraint);
bool AddBreakListener(BreakFn fn);

// BreakConstraint's post patch is on: the witness a drill's break listener waits for.
bool BreakSeamLive();

}  // namespace coop::constraint_ties
