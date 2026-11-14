// coop/gamefix/food_sleep_fix.h -- food and drink give their energy again (game 0.10.0).
//
// mainPlayer_C::addFood ends an eat by adding the item's sleep value, less the player's coffee
// tolerance, to the player's sleep. 0.9.0n added it to the save object's `sleep`. 0.10.0 moved the
// stats onto the player (`save_food`, `save_health`, `save_sleep`) and its addFood adds the gain to
// its own `sleep` parameter instead, so coffee, chocolate and energy drinks fill hunger and leave
// the sleep bar where it was. This lane adds the gain to `save_sleep` as the call ends, by 0.9.0n's
// arithmetic, for an eat the game accepted (its `full` answer false). It acts on the machine's own
// player only, while a session holds the script gate, and only on the function body it was written
// for (coop/gamefix/body_guard.h).

#pragma once

#include <cstdint>

namespace coop::gamefix::food_sleep_fix {

// Register the watch. Idempotent. Any thread.
void Install();

// Off leaves the game's own addFood alone: a drill's control arm. On by default.
void SetEnabled(bool on);

// How many eats took their sleep gain from this lane.
uint64_t Applied();

}  // namespace coop::gamefix::food_sleep_fix
