// coop/dev/director/stage.h -- where a walk starts. A fresh game puts both players at the KPP gate, about 770 m of
// forest from the base, and a drill whose target is in the base would walk all of it. The stage decides, from the
// NavMesh route lengths, whether the walker is put at a known-good anchor nearer the goal first: the base bed spot
// (shelter_spot::Choose) or the KPP gate itself, the two spots the game's out-of-bounds check has already accepted.
// Nothing computed is ever a teleport target. The teleport is the game's own teleportWObackrooms
// (teleport_client::ApplyLocally), which any role may call. Dev only.
//
// Place iff the walker's route to the goal is longer than 100 m AND the best anchor's route is shorter by more than
// 50 m. A walker with no route is judged by its straight distance in the same two tests. Never staged: a straight
// walk (no route to shorten) and a carry walk (the teleport would drop and re-grab the held prop). Turned off for a
// run that measures the walk itself with VOTVCOOP_DRILL_PLACE=0, and for one walk by DirectorGoal::walkFromHere.
// A walk whose epoch has moved or that was cancelled fails the goal ("walks_ended" / "cancelled") and moves nobody.

#pragma once

#include "coop/dev/director/director.h"

namespace coop::director {

// Called by ControlManager::Run before its first tick, on the calling thread (never the game thread): it
// posts one hop per route query (RouteInLegs: up to 16 path queries) and one for the teleport, and polls the
// landing. `why` names the walk in the one log line the stage prints. A caller that needs the route that
// remains, as a deadline, calls it first: goal.staged makes Run's own call a no-op. False, with goal.failed
// set, when the teleport did not land or a hop faulted; true otherwise, whether it placed or not.
bool StageWalk(DirectorGoal& goal, const char* why);

}  // namespace coop::director
