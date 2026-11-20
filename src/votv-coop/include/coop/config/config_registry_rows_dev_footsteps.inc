// coop/config/config_registry_rows_dev_footsteps.inc -- the [dev] rows of the remote player's footsteps: the ground
// line a step may say, and (below it) the crouch drill and its red arm. Included by config_registry_rows_dev.inc at its
// end, so each row keeps its place in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(step_ground_log, "step_ground_log", "dev", false, "VOTVCOOP_STEP_GROUND_LOG",
    "Diagnostic (every peer to be read): a remote player's step reads back whether its ground trace found ground, and "
    "says the first step, every 25th step and every 25th silent step per remote player, as 'puppet_footsteps: slot N "
    "stepped at ..., ground hit|missed'. Off, the trace's hit is not read and no line is said. Read-only.")
CFG_FLAG(vent_crawl_drill, "vent_crawl_drill", "dev", false, "VOTVCOOP_VENT_CRAWL_DRILL",
    "Drill, a crouched player's steps on the other peer; set on BOTH peers (coop/dev/vent_probe). The host crouches, "
    "says so, and once the client has noted how many steps the host's puppet has taken, walks a NavMesh route of 20 to "
    "50 m through the bot director; the client counts the puppet's steps over the distance the host travelled and "
    "judges the spacing: within 20% of the crouched stride (75 cm) is PASS, within 20% of the standing one (150 cm) is "
    "FAIL, fewer than 8 steps or any other spacing is INVALID. 'rig: VERDICT vent-crawl PASS|FAIL|INVALID' and "
    "'[VENT-DRILL] [H|C] DONE'. Red arm: crouch_bit_off. The host's player stays crouched for the rest of the run.")
CFG_FLAG(crouch_bit_off, "crouch_bit_off", "dev", false, "VOTVCOOP_CROUCH_BIT_OFF",
    "Developer red arm of the vent crawl drill (set on the HOST): never set. The pose this peer sends never carries the "
    "crouched bit, so the other peer's puppet of it steps at the standing stride (150 cm) while the player crawls: the "
    "drill must say FAIL.")
