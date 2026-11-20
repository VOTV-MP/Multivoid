// coop/config/config_registry_rows_dev_drive_box.inc -- the [dev] rows of the drive box drill (coop/dev/drive_box_drill):
// its arm and its red. Included by config_registry_rows_dev.inc after the robot cleaner's rows, so each row keeps its
// place in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(drive_box_drill, "drive_box_drill", "dev", false, "VOTVCOOP_DRIVE_BOX_DRILL",
    "Drill, a drive box taken out of the inventory keeps its drives; set on BOTH peers, and VOTVCOOP_CAN_DIR on both. "
    "The host drops a drive box (prop_driveRack_C) and fills 12 of its 16 rows; the client walks to it with the "
    "director, pockets it and takes it back out of its inventory, which places it in the world: the box the host "
    "spawns from that placement must hold the pocketed rows on the host and on the client: 'rig: VERDICT drive-box "
    "PASS|FAIL|INVALID'. It MUTATES the world (a box is dropped). Run it on a fresh host world.")
CFG_FLAG(drive_box_restore_off, "drive_box_restore_off", "dev", false, "VOTVCOOP_RACK_RESTORE_OFF",
    "Developer red arm of the drive box drill: never set; the host. A box a client placed after taking it out of its "
    "inventory is not given back the rows it was pocketed with, so it stands empty on every peer and the drill must FAIL "
    "at the host's reading of the box.")
