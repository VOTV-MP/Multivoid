// coop/config/config_registry_rows_dev_cargo_lift.inc -- the [dev] rows of the cargo lift drill (its arm and its
// red). Included by config_registry_rows_dev.inc after the disc rows. The entry forms are config_registry_rows.inc's.

CFG_FLAG(cargo_lift_drill, "cargo_lift_drill", "dev", false, "VOTVCOOP_RUN_CARGO_LIFT",
    "Drill (BOTH peers): the base's cargo lift is the host's on every peer. The host names its lift, the client walks "
    "to it with the director, the host presses it and the client presses it; each peer waits for its own copy to stop "
    "and the two compare direction and platform height. The host's `rig: VERDICT cargo-lift` ends it.")
CFG_FLAG(cargo_lift_client_runs, "cargo_lift_client_runs", "dev", false, "VOTVCOOP_CARGO_LIFT_CLIENT_RUNS",
    "Developer red arm of the cargo lift drill: never set. A client's press runs the lift's own body on its copy "
    "alone instead of asking the host, so the host's lift and the client's end a press apart.")
