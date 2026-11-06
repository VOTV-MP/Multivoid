// coop/dev/cremator_drill.h -- [dev] the cremator drill (MUTATES the world: the host pulls the lever).
//
// Arms, judged on the HOST from what the client signals through the peers' shared folder (VOTVCOOP_CAN_DIR), each
// wait on a game state with a bound and never on a clock. VOTVCOOP_CREMATOR_DRILL=1:
//   G1  the host pulls the lever: the client's copy reads isActive 1 and its door latched (isClosed 1) once its
//       handle has finished; the host pulls it back: the copy reads 0.
//   G4  with `--rejoin`: the host keeps it on; the client leaves, comes back (VOTVCOOP_CREMATOR_DRILL_REJOIN=1), and
//       its copy must read active after its world-ready.
//
// g3, g3low and g5 run a burn alone: the host sends the client to the firebox with the admin verb's teleport order,
// judges the landing by the cremator's begin-overlap, shuts the door and pulls the lever. g3: the client dies by the
// machine. g3low: the same with the host's own health set to 5 before the lever; the host must refuse the burn loop's
// achievement call for the puppet. g5: the same, the client's own copy watched (actors, refused collections, fireballs).

#pragma once

// The cremate arms ask the /cremate verb through the command dispatcher (command_sync), judge its end line and the
// landing, and read the end reason the client was kicked with. cremate: the client is cremated and kicked with MV-H35;
// cremate-running: the same into a cremator the host already runs; cremate-survive: the host pulls the lever back and
// the client lives; cremate-transit: the client dies before landing; cremate-dropped: the order never applies. The
// refusal arms (cremate-exempt with the rig's --observer, cremate-self, cremate-host) ask from client 1, granted the
// node by the host's own /mv, and judge the refusal line.
//
// Lines: "cremator-drill: VERDICT PASS|FAIL g1-on|g1-off|g4|g3|g3low|g5|<cremate arm> ...", "G1|G3|G3LOW|G5|<CREMATE ARM>
// DONE" (host), "DONE on host|client", "REJOIN DONE", "VERDICT ABORT ...". Red arms: the host's cremator_state_off=1
// (G1, G4), cremator_hit_off=1 (G3), player_damage_no_refusal=1 (G3LOW) and cremate_landing_off=1 (cremate); the
// client's cremator_client_guard_off=1 (G5). The client's knobs: cremate_transit_die, cremate_drop_order.

#include <windows.h>

namespace coop::dev::cremator_drill {

// The drill's thread, spawned by the harness when the row is set; one per process, the role read from the config.
DWORD WINAPI Thread(LPVOID arg);

}  // namespace coop::dev::cremator_drill
