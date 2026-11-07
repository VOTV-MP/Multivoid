// coop/dev/mannequin_walker_steps_b.h -- [dev] the walker drill's second increment: steps 6, 5 and 7 (numbered as in mannequin_walker_drill.h; step 3 is absent).
//
// The drill (coop/dev/mannequin_walker_drill) runs steps 1, 2 and 4 on walker A and walker B; these continue it, in the order they
// run (there is no step 3: see mannequin_walker_drill.h):
//   6 HIT   (walker B, the client its target, after step 4's release) the host angers B with its own fireDamage (only an angry walker
//           hits); the client keeps its view away and stands, B reaches it and hits, and the client counts a walker hit delivered to
//           it and a fall of its health, then leaves at once. Red: no_delivery.
//   5 LOSS  (B alive and still hunting the client, or INVALID) the client leaves the session (network first, as a quit does) and prints
//           the rejoin marker; on the host B is told its target is lost within a bound (the notice, not a later re-pick). Red: no_loss.
//   7 JOIN  the client comes back as a new process (the rig's --rejoin; the second life carries VOTVCOOP_MANNEQUIN_DRILL_REJOIN=1
//           and finds its place by its own start, peer_signal marks being those written since it began); the host reads B's target
//           and `looking`, the client reads the state row it was sent for B's key within 5 s of its own world-ready, and the host compares
//           them. Red: no_join_state. Step 8, the statue route, follows in the rejoined life (coop/dev/mannequin_walker_statue).
// The host judges; a bound that runs out is an INVALID, and a measured failure ends the run at its step as the verdict.

#pragma once

#include <string>

namespace coop::dev::mannequin_walker_drill::steps_b {

// Steps 6, 5 and 7, the host's half, after step 4: `b` is walker B, whose target is the client. `clientFails` receives the failure
// count the rejoined client reports.
bool HostB(void* b, const std::string& keyB, int& clientFails);

// Steps 6 and 5, the client's half, after step 4: the first life ends here, after printing the rejoin marker the rig restarts it on.
bool ClientB(const std::string& keyB);

// Whether this process is the client's second life (the rig's --rejoin-env), and what it runs: step 7's reading.
bool IsRejoinedLife();
void RunRejoin();

}  // namespace coop::dev::mannequin_walker_drill::steps_b
