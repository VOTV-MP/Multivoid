// coop/dev/grants_drill.h -- drill: this machine's local dev grants are read after they are applied
// and judged against an arm's expectation (ini grants_drill=grant|none|hostdeny, expectation
// grants_drill_expect; env VOTVCOOP_GRANTS_DRILL, VOTVCOOP_GRANTS_DRILL_EXPECT; BOTH peers).
//
// What each arm expects, per peer:
//   grant     the client holds the position readout alone; the host holds all five
//   none      the client holds none; the host holds all five
//   hostdeny  the client holds none; the host holds all but the position readout
// The rig's permission fixtures (the host's server folder) make the arm true; the drill only reads.
// A client judges once per session, when its grants for that session are in. The host judges when
// its own are in and a client is paired (roster count >= 2), and again on each rising edge of that
// pairing: a joiner relaunched after a boot flake re-marks the host's log, and the verdict must
// follow the mark. `grants_drill_expect` set to another arm judges against that arm (the red).
// [GRANTS-DRILL]: '<role> PASS (bits=0x..)' or '<role> FAIL: <node> is <0|1>' (the first mismatch
// in the projected table's order); <role> is `host` or `c<slot>`.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::grants_drill {

// Every pump tick in a world; a latched read when off. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::grants_drill
