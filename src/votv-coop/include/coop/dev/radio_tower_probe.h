// coop/dev/radio_tower_probe.h -- dev-only instrument for the radio tower (ini radio_tower_probe=<mode> /
// env VOTVCOOP_RADIO_TOWER_PROBE, empty = off; BOTH peers).
//
// observe: once the gamemode's radio tower is live, says what it is (where, its puzzle's size, its fuse and
// puzzle counts), prints its state once a second when it changed, and name-watches the tower's bodies at the
// script gate, counting each call per peer. Read-only; it never refuses a call.
// fixclient: observe, plus a fixture. The host breaks the tower once a client's world is ready; the client
// completes the puzzle on its own copy. It is the measurement taken on b256, where that repair stayed on the
// client and the host kept reading the tower broken; the wait it makes is a clock, since a probe measures.
// Lines tagged [RADIO-TOWER]; the host's DONE line ends the run.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::radio_tower_probe {

// Register the watches and hold the gate, then read the tower once a second. Every pump tick, from the
// load window's call (the tower's own timers and the load run before the world is up). Game thread.
void Tick(const coop::net::Session& session);

}  // namespace coop::dev::radio_tower_probe
