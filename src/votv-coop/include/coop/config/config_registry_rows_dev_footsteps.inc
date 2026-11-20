// coop/config/config_registry_rows_dev_footsteps.inc -- the [dev] rows of the remote player's footsteps: the ground
// line a step may say, and (below it) the crouch drill and its red arm. Included by config_registry_rows_dev.inc at its
// end, so each row keeps its place in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(step_ground_log, "step_ground_log", "dev", false, "VOTVCOOP_STEP_GROUND_LOG",
    "Diagnostic (every peer to be read): a remote player's step reads back whether its ground trace found ground, and "
    "says the first step, every 25th step and every 25th silent step per remote player, as 'puppet_footsteps: slot N "
    "stepped at ..., ground hit|missed'. Off, the trace's hit is not read and no line is said. Read-only.")
