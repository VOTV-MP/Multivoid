// coop/dev/load_reroll_watch.h -- dev-only, read-only instrument for what a world load rolls again
// (ini load_reroll_watch=1 / env VOTVCOOP_LOAD_REROLL_WATCH, off by default; BOTH peers).
//
// Two bodies can re-roll shared state inside a world load, where no pump of ours drains: the
// server-upgrade spawner's upgrades() (after the gamemode's begin-play reaches it) and a
// coordinate tower's Scramble Radar Dish (from its own loadData when it loads broken, from its
// breakdown timer, or from the generator saboteur). Both are name-watched at the script gate from
// the pump's first tick -- on a joining client that is before its world -- so the watch is live
// when the join's load runs. The probe holds the gate switch on while its flag is, with or without
// a session, so for that run every lane's gate watch also fires outside a session; it says at each
// world change whether the switch was found off. The first 32 entries are logged with the role,
// the join phase and the caller, and every entry is counted. It never refuses.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::load_reroll_watch {

// Register, hold the gate switch, and report each world change. Every pump tick, with a session or
// without one (a world loaded from the menu before any session is seen only from the latter; a
// boot that loads its world in one blocking call is seen by neither), and OUTSIDE the world-up
// gate: the loads it watches happen while no local player exists. Game thread.
void Tick(const coop::net::Session& session);

}  // namespace coop::dev::load_reroll_watch
