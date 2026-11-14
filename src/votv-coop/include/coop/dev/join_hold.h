// coop/dev/join_hold.h -- the rejoined observer's two holds of the ATV riders drill (coop/dev/atv_riders_drill). In the life
// the rig restarts (VOTVCOOP_ATV_RIDERS_REJOIN=1, row on) the driver's puppet is not born and the NPC mirrors are not bound
// until the drill releases them, on the observer's first applied ATV stamp: so the stamp always comes first and the owners'
// own ready edges (the birth, the bind) are what apply it. Off in every other run, and once released for the life.
// Read by coop/player/puppet_drive and coop/creatures/npc_adoption; released by the drill's thread. Atomics.

#pragma once

#include <cstdint>

namespace coop::dev::join_hold {

inline constexpr uint8_t kDriverSlot = 2;   // the slot whose puppet is held

// This is the rejoined life of a drill run (the environment's word, read once).
bool RejoinedLife();

// The puppet of `slot` is held unborn.
bool BirthHeld(uint8_t slot);
// A pending NPC mirror bind (adoption or fresh spawn) is held.
bool BindHeld();

void ReleaseBirth();
void ReleaseBind();

}  // namespace coop::dev::join_hold
