// coop/dev/pried_measure.h -- [dev] the pry drill's MEASURE arm: one plank or one radiator pried by the client, every
// step logged on both peers, then a client that joins afterwards logs what it sees of the same prop. It judges
// nothing; it ends on `rig: VERDICT pry-measure DONE`, or `INVALID -- <why>` when a wait ran out.
//
// Every line carries the PRY-MEASURE tag, a millisecond clock (one for both peers on a machine) and the peer's H or C.
//   BEFORE, REST, JOINER-READ, HOST-AT-JOINER: the static, frozen, sleep, simulating and awake flags, the pose, and
//        one PART line per member component (class, name, pose, simulating, a constraint's IsBroken);
//   crowbarOpen, breakingSequence and ExecuteUbergraph (with its entry point) PRE|POST, and BreakConstraint (a native
//        post hook): each call on the pried prop, with the flags;
//   SAMPLE every 500 ms until 5 s after the last of those events: pose, flags, constraints broken, parts moved.
// Flow: the host names the prop, the client pries it with the prop's own crowbarOpen (the pry lane carries it to the
// host), both sample until quiet, the client prints the rejoin marker, and the rig's --rejoin relaunches it with
// VOTVCOOP_PRIED_MEASURE_REJOIN=1: that life reads and samples its copy, then the host reads its own and ends.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::dev::pried_measure {

// One pass on either peer, after the pried_drill row chose this arm. `radiator` selects the class and the extra
// watches. Game thread.
void Tick(coop::net::Session& session, bool radiator);

// Back to the first step. Net disconnect.
void OnDisconnect();

}  // namespace coop::dev::pried_measure
