// coop/dev/mannequin_walker_steps_b.h -- [dev] the walker drill's second increment: steps 3, 6, 5 and 7 (the sheet's numbers).
//
// The drill (coop/dev/mannequin_walker_drill) runs steps 1, 2 and 4 on walker A and walker B; these continue it, in the order they
// run:
//   3 KEEP  (walker A, the host its target) the client walks until it stands nearer to A than the host by 2000 cm, read while it
//           walks; then, for 5 s, every host read of the walker's target stays the host. Red: no_keep.
//   6 HIT   (walker B, the client its target, after step 4's release) the client keeps its view away and stands; B reaches it and
//           hits, and the client counts a walker hit delivered to it and a fall of its health. Red: no_delivery.
//   5 LOSS  the client leaves the session (network first, as a player's quit does) and prints the rejoin marker; on the host B's
//           target stops being the client within a bound. Red: no_loss.
//   7 JOIN  the client comes back as a new process (the rig's --rejoin; the second life carries VOTVCOOP_MANNEQUIN_DRILL_REJOIN=1
//           and finds its place by its own start, peer_signal marks being those written since it began); the host reads B's target
//           and `looking`, the client reads the state row it was sent for B's key, and the host compares them. Red: no_join_state.
// The host judges. A client's step reports what it read at a mark; a bound that runs out is an INVALID, and a measured failure ends
// the run at its step as the verdict. Every wait is on a state, with a named bound.

#pragma once

#include <string>

namespace coop::dev::mannequin_walker_drill::steps_b {

// Step 3, the host's half, after step 2 passed: `a` is the walker whose copy step 2 followed. False when the run must end (a verdict
// was said).
bool HostKeep(void* a);

// Step 3, the client's half: the copy of walker A is held here under `keyA`.
bool ClientKeep(const std::string& keyA);

// Steps 6, 5 and 7, the host's half, after step 4: `b` is walker B, whose target is the client. `clientFails` receives the failure
// count the rejoined client reports.
bool HostB(void* b, const std::string& keyB, int& clientFails);

// Steps 6 and 5, the client's half, after step 4: the first life ends here, after printing the rejoin marker the rig restarts it on.
bool ClientB(const std::string& keyB);

// Whether this process is the client's second life (the rig's --rejoin-env), and what it runs: step 7's reading.
bool IsRejoinedLife();
void RunRejoin();

}  // namespace coop::dev::mannequin_walker_drill::steps_b
