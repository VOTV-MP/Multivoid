// coop/dev/sleep_speedup_drill.h -- [dev] drill: whose sleep speeds the world up (ini sleep_speedup_drill=
// modes|row_change|leave / env VOTVCOOP_SLEEP_SPEEDUP_DRILL on BOTH peers, the peers' folder VOTVCOOP_CAN_DIR,
// VOTVCOOP_FILL_NEEDS=0, a fresh host world, the host's row by `--host-config world.sleep_speedup=<mode>`). The host
// judges and says `rig: VERDICT sleep-speedup PASS|FAIL -- <arm>: <what>`; a bound that runs out, or a dead sleep or
// wakeup watch, says INVALID. The red `sleep_speedup_rule_off` makes the host ignore the row and act as `host`: the
// everyone and never arms must FAIL at the first read.
#pragma once

// The host, once a client's join is over and no world event is active, goes to bed through the game's own sleep with
// its need held at 50, and reads the world's dilation at each edge below, each read in the tick that sees the edge:
//   modes, everyone: right after its sleep call, the client awake: 1. The client's rest state rises (it sleeps once it
//     sees the host asleep): the game's factor. The client wakes (told by a signal once the host has read): 1.
//   modes, host:  right after its sleep call: the game's factor, whoever is awake. The client does nothing.
//   modes, never: right after its sleep call: 1. The client's rest state rises: still 1.
//   row_change (the row everyone): the host asleep, the client awake: 1; the row set to host for the session: the
//     game's factor at once; the row set back to everyone: 1. The client does nothing.
//   leave (the row everyone or never): both asleep as in the modes; then the client leaves the session asleep and the
//     host reads once its roster row is gone: everyone the game's factor, never 1.

namespace coop::net { class Session; }

namespace coop::dev::sleep_speedup_drill {

// Per frame, both roles. A latched read when the drill is off.
void Tick(coop::net::Session* session);

// The session ended: the steps start over.
void OnDisconnect();

}  // namespace coop::dev::sleep_speedup_drill
