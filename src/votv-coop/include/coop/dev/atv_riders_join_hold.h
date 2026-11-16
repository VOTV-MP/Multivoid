// coop/dev/atv_riders_join_hold.h -- the three holds of the ATV riders drill (coop/dev/atv_riders_drill).
//
// The rejoined observer's two: in the life the rig restarts (VOTVCOOP_ATV_RIDERS_REJOIN=1, row on) the driver's puppet is not
// born and the adopted NPC twins are not bound until the drill releases them, on the observer's first applied ATV stamp: so the
// stamp always comes first and the owners' own ready edges (the birth, the bind) are what apply it. Off in every other run, and
// once released for the life. The adoption scan (coop/creatures/npc_adoption asks BindHeld) waits; a fresh-spawn mirror is never
// held: it binds inside the join's snapshot, hidden by coop/element/mirror_defer, and its ride starts at the reveal. Read by
// coop/player/puppet_drive and coop/creatures/npc_adoption; released by the drill's thread. Both holds are atomics.
//
// The driver's lift (the values that run step 0, in the driver's first life): the join's curtain-lift reveal and its quiescence
// reveal (mirror_defer) wait until the kerfur the host's stamp names is bound, so the mirror is born hidden and the ride starts at
// the reveal, by the event and not by the order two arrivals happen to take. The stamp's first kerfur row names the eid
// (NoteRideEid); its bind (OnKerfurBound) or the drill's own bound (ReleaseLift) ends the hold and runs the reveals.

#pragma once

#include <cstdint>

namespace coop::dev::atv_riders_join_hold {

inline constexpr uint8_t kDriverSlot = 2;   // the slot whose puppet is held, and whose lift is

// This is the rejoined life of a drill run (the environment's word, read once).
bool RejoinedLife();

// The puppet of `slot` is held unborn.
bool BirthHeld(uint8_t slot);
// The adoption scan's NPC bind is held.
bool BindHeld();

void ReleaseBirth();
// Lets the adoption scan go. Any thread.
void ReleaseBind();

// The driver's lift: registers the hold with mirror_defer when this run holds it (the row runs step 0, first life); a run without
// it registers nothing. Game thread, at the session's install.
void InstallLiftHold();
// A kerfur row of the host's stamp reached npc_ride (an outcome of rides, held unbound or held hidden): the first one names the
// kerfur the hold waits for. `bound` says its mirror is bound already, so the hold ends in a task posted to the game thread.
// Game thread.
void NoteRideEid(uint32_t eid, bool bound);
// A kerfur mirror was bound (npc_ride::OnMirrorBound): when it is the named one the hold ends and the reveals that waited run.
// Game thread.
void OnKerfurBound(uint32_t eid);
// The drill's bound: ends the hold whatever the bind did, and the reveals run in a task on the game thread. Any thread.
void ReleaseLift();

}  // namespace coop::dev::atv_riders_join_hold
