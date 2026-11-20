// coop/dev/drive_box_drill.h -- [dev] a drive box taken out of the inventory keeps its drives (the drive_box_drill row,
// VOTVCOOP_DRIVE_BOX_DRILL, both peers, VOTVCOOP_CAN_DIR on both; MUTATES the world: the host drops a box).
//
// Arm 1: the host drops a drive box (prop_driveRack_C), writes all 16 rows at the wire's caps and tells the client. The
// client walks to the host with the director, waits for its copy to hold the same rows, checks the box's record against
// the hand lane's bounds and byte ceiling, pockets it and takes it back out of its inventory, which places it in the
// world. The host spawns a box from that placement (a drop intent naming the pocketed key): at the rack lane's first
// sight of it the rows are the pocketed rows or they are not, and the host says FAIL there. Then the client waits for the
// host's canonical of that box and compares its own box's rows. `rig: VERDICT drive-box PASS|FAIL|INVALID`. The red is
// drive_box_restore_off on the host: the box comes back empty, FAIL at the host's reading.
// Arm reuse (the host alone, the client joins): a box that went holding rows leaves them with the lane under its key, and a
// new empty box under that key which no client placed must stay empty. `rig: VERDICT drive-box-reuse`. The red is
// drive_box_placed_gate_off on the host: the new box is filled, FAIL.
//
// Lines: "drive-box: ..." (progress), "rig: VERDICT drive-box ..." (the verdict).

#pragma once

#include <windows.h>

namespace coop::dev::drive_box_drill {

// The drill's thread, spawned by the harness when the row is set; one per process, the role read from the config.
DWORD WINAPI Thread(LPVOID arg);

}  // namespace coop::dev::drive_box_drill
