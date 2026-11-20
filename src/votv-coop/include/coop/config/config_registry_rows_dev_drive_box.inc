// coop/config/config_registry_rows_dev_drive_box.inc -- the [dev] rows of the drive box drill (coop/dev/drive_box_drill):
// its arms and its reds. Included by config_registry_rows_dev.inc after the robot cleaner's rows, so each row keeps its
// place in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(drive_box_drill, "drive_box_drill", "dev", "off", "off|1|reuse", "VOTVCOOP_DRIVE_BOX_DRILL",
    "Drill, a drive box taken out of the inventory keeps its drives; set on BOTH peers, and VOTVCOOP_CAN_DIR on both. "
    "1 -- the host drops a drive box (prop_driveRack_C) and fills all 16 of its rows with texts at the wire's caps; the "
    "client walks to it with the director, checks the box's record against the hand lane's bounds and byte ceiling, pockets "
    "it and takes it back out of its inventory, which places it in the world: the box the host spawns from that placement "
    "must hold the pocketed rows on the host and on the client: 'rig: VERDICT drive-box PASS|FAIL|INVALID'. reuse -- "
    "the host drops and fills a box, destroys it (the lane keeps its rows by key), then spawns a new empty box under the "
    "same key, which no client placed: it must stay empty ('rig: VERDICT drive-box-reuse'); the client only joins. It "
    "MUTATES the world (a box is dropped). Run it on a fresh host world.")
CFG_FLAG(drive_box_restore_off, "drive_box_restore_off", "dev", false, "VOTVCOOP_RACK_RESTORE_OFF",
    "Developer red arm of the drive box drill (1): never set; the host. A box a client placed after taking it out of its "
    "inventory is not given back the rows it was pocketed with, so it stands empty on every peer and the drill must FAIL "
    "at the host's reading of the box.")
CFG_FLAG(drive_box_placed_gate_off, "drive_box_placed_gate_off", "dev", false, "VOTVCOOP_DRIVE_BOX_PLACED_GATE_OFF",
    "Developer red arm of the drive box drill (reuse): never set; the host. A box is given a gone box's rows at its first "
    "sight whether or not a client placed it, as before the placement was the event, so the new empty box under the "
    "pocketed key is filled and the drill must FAIL.")
