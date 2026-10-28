// coop/config/config_registry_rows_dev_server.inc -- the [dev] rows of the server boxes' drills: the repair a
// client runs on the host's box, and an upgrade's install and take-out. Included by config_registry_rows_dev.inc
// before the PI and NB rows, which stay the last ones. The entry forms are config_registry_rows.inc's.

CFG_STRING(server_drill, "server_drill", "dev", "", "VOTVCOOP_SERVER_DRILL", false,
    "Drill (BOTH peers): a server box's break is the host's and a client's repair of it runs there. The host breaks "
    "a box with no upgrades and stamps its repair type 123. run -- once a client is in its world: the client finds "
    "it broken with that type, its own breakServer on a healthy box is refused, it walks to the box and runs the "
    "repair widget's end(true), and the host's box ends fixed with the repair's pay (15, 50 in lol mode) on both "
    "balances; runred -- the same, but the client ends the widget with a failed repair, which pays nothing, so the "
    "money check must FAIL; join -- the break in the joiner's window, found after its world is ready. Lines tagged "
    "[SERVER-DRILL]; the client's DONE line ends it.")
CFG_STRING(server_upgrade_drill, "server_upgrade_drill", "dev", "", "VOTVCOOP_SERVER_UPGRADE_DRILL", false,
    "Drill (BOTH peers): a client installs an upgrade into a server box and takes one out. apply: the host "
    "applies both; refuse: the host refuses both, as if each had lost its race, and the client sees its install "
    "refunded and its take-out's upgrade go. Lines tagged [SRV-UPG-DRILL].")
