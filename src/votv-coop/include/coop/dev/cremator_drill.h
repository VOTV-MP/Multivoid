// coop/dev/cremator_drill.h -- [dev] the cremator drill (MUTATES the world: the host pulls the lever).
//
// Arms, judged on the HOST from what the client signals through the peers' shared folder (VOTVCOOP_CAN_DIR), each
// wait on a game state with a bound and never on a clock. VOTVCOOP_CREMATOR_DRILL=1:
//   G1  the host pulls the lever: the client's copy reads isActive 1 and its door latched (isClosed 1) once its
//       handle has finished; the host pulls it back: the copy reads 0.
//   G4  with `--rejoin`: the host keeps it on; the client leaves, comes back (VOTVCOOP_CREMATOR_DRILL_REJOIN=1), and
//       its copy must read active after its world-ready.
// g3 and g5 run a burn alone: the host sends the client to the firebox with the admin verb's teleport order
// (`/cremate`'s move), judges the landing by the cremator's begin-overlap, shuts the door and pulls the lever. g3: the
// client dies by the machine. g5: the same, with the client's own copy watched -- its `actors` reads 0 at every look,
// the client guard refused a collection, and the fireball births are reported, not judged.
// Lines: "cremator-drill: VERDICT PASS|FAIL g1-on|g1-off|g4|g3|g5 ...", "G1|G3|G5 DONE" (host), "DONE on host|client",
// "REJOIN DONE", "VERDICT ABORT ...". Red arms: the host's cremator_state_off=1 (G1, G4) and cremator_hit_off=1 (G3);
// the client's cremator_client_guard_off=1 (G5).

#pragma once

#include <windows.h>

namespace coop::dev::cremator_drill {

// The drill's thread, spawned by the harness when the row is set; one per process, the role read from the config.
DWORD WINAPI Thread(LPVOID arg);

}  // namespace coop::dev::cremator_drill
