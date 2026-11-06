// coop/dev/cremator_drill.h -- [dev] the cremator drill (MUTATES the world: the host pulls the lever).
//
// Arms, judged on the HOST from what the client signals through the peers' shared folder (VOTVCOOP_CAN_DIR, the
// peer_signal handshake), each wait on a game state with a bound and never on a clock. VOTVCOOP_CREMATOR_DRILL=1:
//   G1  the host pulls the lever: the client's copy reads isActive 1 and its door latched (isClosed 1) once its
//       handle has finished; the host pulls it back: the copy reads 0.
//   G4  with `--rejoin`: the host pulls it again and keeps it on; the client leaves, comes back as a new process
//       (VOTVCOOP_CREMATOR_DRILL_REJOIN=1), and its copy must read active after its world-ready.
// VOTVCOOP_CREMATOR_DRILL=g3 runs the burn alone: the host sends the client to the firebox with the admin verb's
// teleport order (`/cremate`'s move, the verb under test), judges the landing by the cremator's begin-overlap for the
// client's puppet, shuts the door and pulls the lever; the client waits for its own death and signals it.
// Lines: "cremator-drill: VERDICT PASS|FAIL g1-on|g1-off|g4|g3 ...", "G1 DONE" / "G3 DONE" (host), "DONE on
// host|client", "REJOIN DONE", "VERDICT ABORT ..." (a wait that never ended); the client's "[CREMATOR-HIT] applied
// verb=damage ... lethal=1 achievement=1" is the achievement. Red arms: the host's cremator_state_off=1 (G1, G4 FAIL)
// and cremator_hit_off=1 (the client keeps its health, G3 FAILs).

#pragma once

#include <windows.h>

namespace coop::dev::cremator_drill {

// The drill's thread, spawned by the harness when the row is set; one per process, the role read from the config.
DWORD WINAPI Thread(LPVOID arg);

}  // namespace coop::dev::cremator_drill
