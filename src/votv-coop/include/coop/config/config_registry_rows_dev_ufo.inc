// coop/config/config_registry_rows_dev_ufo.inc -- the [dev] rows of the ambient UFOs' drill (coop/world/ufo_sync): its
// arm and its red. Included by config_registry_rows_dev.inc after the cargo lift rows, so each row keeps its place in
// every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(ufo_path_send_off, "ufo_path_send_off", "dev", false, "VOTVCOOP_UFO_PATH_SEND_OFF",
    "Developer red arm of the UFO drill: never set; the host. The host sends no UFO path, so a client's copy of a ball "
    "or a boo has none and the drill must FAIL at the Midas run it sends after them.")
CFG_ENUM(ufo_drill, "ufo_drill", "dev", "off", "off|1|join", "VOTVCOOP_UFO_DRILL",
    "Drill, the ambient UFOs' rolled paths; set on BOTH peers, and VOTVCOOP_CAN_DIR on both. 1 -- the host calls the "
    "gamemode's own ufo_ball until a ball stands enrolled, then runs the Midas UFO (its take-off is the "
    "fence on the same ordered lane); the client must have received the ball's path by the Midas run, and the copy must "
    "read the host's path: 'rig: VERDICT ufo PASS|FAIL|INVALID' and 'ufo-drill: DONE on host'. join -- with the rig's "
    "--rejoin: the host holds a ball, the client leaves, and at the rejoined client's ready edge (second life: "
    "VOTVCOOP_UFO_DRILL_REJOIN=1) the host must have sent a path and the copy must read it: 'rig: VERDICT ufo-join "
    "PASS|FAIL|INVALID'. Red arm: ufo_path_send_off on the host. It MUTATES the world (UFOs are minted). Run it on a "
    "fresh host world.")
