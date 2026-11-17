// coop/dev/sleep_speedup_drill.h -- [dev] drill: whose sleep speeds the world up, one arm per value of the host's
// world.sleep_speedup row (ini sleep_speedup_drill=1 / env VOTVCOOP_SLEEP_SPEEDUP_DRILL=1 on BOTH peers, the peers'
// folder VOTVCOOP_CAN_DIR, VOTVCOOP_FILL_NEEDS=0, a fresh host world, the host's row by `--host-config
// world.sleep_speedup=<mode>`). The host judges and says `rig: VERDICT sleep-speedup PASS|FAIL -- <mode>: <what>`;
// a bound that runs out says INVALID.
//
// The host, once a client's join is over and no world event is active, goes to bed through the game's own sleep with
// its need held at 50, and reads the world's dilation at each edge below, each read in the tick that sees the edge:
//   everyone: right after its sleep call, the client awake: 1. The client's rest state rises (it sleeps once it sees
//     the host asleep): the game's factor. The client wakes (it is told by a signal once the host has read): 1.
//   host:     right after its sleep call: the game's factor, whoever is awake. The client does nothing.
//   never:    right after its sleep call: 1. The client's rest state rises: still 1.
// The red `sleep_speedup_rule_off` makes the host ignore the row and act as `host`: the everyone and never arms must
// FAIL at the first read.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::sleep_speedup_drill {

// Per frame, both roles. A latched read when the drill is off.
void Tick(coop::net::Session* session);

// The session ended: the steps start over.
void OnDisconnect();

}  // namespace coop::dev::sleep_speedup_drill
