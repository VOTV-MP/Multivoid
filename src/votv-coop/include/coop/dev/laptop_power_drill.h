// coop/dev/laptop_power_drill.h -- [dev] the stationary laptop's power, pressed on each peer, its replay judged.
// Each peer presses its own laptop with the blueprint's own press (actionOptionIndex, action 8), and each step waits
// on its laptop settling on the value it expects, its bound a failure bound, never a wait.
//   CLIENT -- presses: its one edge goes. The host's press comes back as a replay that sends nothing back. It presses
//             again (a replay that never released the lane would swallow this edge); once the host's second press
//             is replayed, it sends two edges with no toggle, the other value then back: two players' edges crossing.
//   HOST   -- the client's press replayed with no edge of its own; presses, one edge; the client's second replayed;
//             presses again; then the crossing edges, the second setting the target back while the first one's
//             toggle runs, so the laptop must settle where it was, with no edge of its own sent: DONE.
// "[LAPTOP-POWER-DRILL] FAIL" is a step failing (--fail-marker), "[LAPTOP-POWER-DRILL] ABANDONED" the drill unable to
// do its part (--dead-marker), "[LAPTOP-POWER-DRILL] host DONE PASS" the run's end (--done-marker). Run with
// laptop_power_drill=1 on both peers.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::laptop_power_drill {

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// Back to the start: a rejoin's peers run their steps again.
void OnDisconnect();

}  // namespace coop::dev::laptop_power_drill
