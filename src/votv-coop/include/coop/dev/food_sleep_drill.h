// coop/dev/food_sleep_drill.h -- [dev] an eat's sleep gain, measured on each peer's own player
// (coop/gamefix/food_sleep_fix).
//
// Arms (`food_sleep_drill`): `on` -- each peer, once its join is over, sets its own player's sleep
// to 40 and calls the game's addFood three times: sleep 20 with no coffee tolerance (60 wanted),
// sleep 20 with tolerance 0.5 (50 wanted), and a refused eat at full hunger (40 wanted, unchanged).
// It then puts the player's sleep, food and coffee tolerance back. `nofix` -- the same with the fix
// off, the control arm, which must FAIL with the sleep left at 40. One `[FOOD-SLEEP-DRILL] [H|C]
// PASS` or `FAIL` line per peer.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::dev::food_sleep_drill {

bool IsEnabled();

// The drill's pass on either peer. A single read when off. Game thread.
void Tick(coop::net::Session* session);

// Back to the first step. Net disconnect.
void OnDisconnect();

}  // namespace coop::dev::food_sleep_drill
