// coop/config/config_registry_rows_dev_drone.inc -- the [dev] rows of the drone hover drill: its bump arm and its red.
// Included by config_registry_rows_dev.inc after the disc rows. The entry forms are config_registry_rows.inc's.

CFG_FLAG(drone_hover_kick, "drone_hover_kick", "dev", false, "VOTVCOOP_DRONE_HOVER_KICK",
    "Drill arm (the client, with VOTVCOOP_RUN_DRONE_HOVER=1): the client spins its hovering drone copy at 720 deg/s "
    "and pushes it at 300 cm/s twice, as a touch would. A copy out of the physics simulation holds still and the "
    "drill passes; a copy still in it flickers and the drill must say FAIL. Off = no bump.")
CFG_FLAG(drone_hover_body_red, "drone_hover_body_red", "dev", false, "VOTVCOOP_DRONE_HOVER_BODY_RED",
    "Drill red (the client, with VOTVCOOP_RUN_DRONE_HOVER=1 and drone_hover_kick): the client puts its drone copy's "
    "root back into the physics simulation before it samples, the behaviour before the copy left it. The bumped "
    "copy then sits off its path and the host's verdict must be FAIL. Off = the copy stays out of the simulation, "
    "as designed.")
