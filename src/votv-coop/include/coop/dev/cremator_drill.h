// coop/dev/cremator_drill.h -- [dev] the cremator's lever run crosses (MUTATES the world: the host pulls the lever).
//
// Arms, judged on the HOST from what the client's copy signals through the peers' shared folder (VOTVCOOP_CAN_DIR, the
// peer_signal handshake), each wait on a game state with a bound and never on a clock:
//   G1  the host pulls the lever on its copy: the client's copy reads isActive 1 and its door latched (isClosed 1)
//       once its handle has finished; the host pulls it back: the client's copy reads 0.
//   G4  with `--rejoin`: the host pulls the lever again and keeps it on; the client leaves, comes back as a new
//       process (its second life carries VOTVCOOP_CREMATOR_DRILL_REJOIN=1), and its copy must read active after its
//       world-ready.
// Lines: "cremator-drill: VERDICT PASS|FAIL g1-on|g1-off|g4 ...", "cremator-drill: G1 DONE" (host, after G1),
// "cremator-drill: DONE on host|client" (the last arm run), "cremator-drill: REJOIN DONE" (the second life), and
// "cremator-drill: VERDICT ABORT ..." from a wait that never ended.
// Run: VOTVCOOP_CREMATOR_DRILL=1 on both peers. Without --rejoin end the run on 'cremator-drill: G1 DONE'. The red
// arm is the host's cremator_state_off=1: the client's copy stays idle and G1 and G4 FAIL.

#pragma once

#include <windows.h>

namespace coop::dev::cremator_drill {

// The drill's thread, spawned by the harness when the row is set; one per process, the role read from the config.
DWORD WINAPI Thread(LPVOID arg);

}  // namespace coop::dev::cremator_drill
