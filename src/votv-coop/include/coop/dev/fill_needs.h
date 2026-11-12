// coop/dev/fill_needs.h -- [dev] the local player neither hungers nor tires while a drill runs.
//
// A drill that is not about food, sleep or fainting must not have its player faint halfway: the game's food and sleep
// ticker ragdolls a player whose save slot's `food` or `sleep` reaches 0. With fill_needs=1 (env VOTVCOOP_FILL_NEEDS)
// each peer, once its world is up, writes both to 100 through ue_wrap/actors/vitals and again every 20 s. Local only:
// nothing crosses the wire, unlike vitals_keepalive_sec, which is the host's and refills every peer. A drill about
// food, sleep or fainting leaves it off. Run: --config fill_needs=1 (both peers).

#pragma once

namespace coop::dev::fill_needs {

// Game thread, once per pump tick; a single latched read when off.
void Tick();

}  // namespace coop::dev::fill_needs
