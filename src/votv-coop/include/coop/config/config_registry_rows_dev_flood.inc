// coop/config/config_registry_rows_dev_flood.inc -- the [dev] rows of the basement flood lane: its drill and its three red
// arms. Included by config_registry_rows_dev.inc after the footsteps rows. The entry forms are config_registry_rows.inc's.

CFG_ENUM(flood_drill, "flood_drill", "dev", "off", "off|burst|mend|far|join|mendold", "VOTVCOOP_FLOOD_DRILL",
    "Drill (BOTH peers, default off; run with mp.py smoke on a fresh host): the basement flood is the host's on every peer, "
    "each arm ending on rig: VERDICT flood-<arm> PASS, FAIL or INVALID. burst: the host bursts its first pipe on the game's own "
    "addDamage; the client's copy must read every pipe as the host's and, once the host's level has risen, its level within the "
    "lane's step and the rise between two reads. mend: the burst, then the client is walked to the pipe by the director, takes a "
    "toolbox into its hand and runs the pipe's toolboxFix; the host's pipe must read mended at its judge, and when its level has "
    "fallen the client's copy must read the host's pipes and level. far: the burst, then the client, standing out of reach, sends "
    "a raw FloodIntent with a toolbox in its hand; the host must refuse it for the target check and leave its pipe burst. join: "
    "the burst; the client then leaves (set --rejoin with --rejoin-marker \"[FLOOD-DRILL] CLIENT LIFE DONE\" and --rejoin-env "
    "VOTVCOOP_FLOOD_DRILL_REJOIN=1); once the rejoining client's world is captured and before it is ready the host bursts a "
    "second pipe, which only the ready edge's replay can carry, and the joiner's pipes and level must read the host's at its "
    "world-ready. mendold: the burst, then the host forgets which bit each pipe's last send carried (as a pipe burst before "
    "the session's first send) and mends the pipe itself; the client's pipes must read the host's.")
CFG_FLAG(flood_state_off, "flood_state_off", "dev", false, "VOTVCOOP_FLOOD_STATE_OFF",
    "Developer red arm of the flood drill (host): never set. The host sends no change of a pipe or of the level to the clients "
    "(a joiner's replay and the answer to a refused repair still go), so burst must say FAIL at the client's first read.")
CFG_FLAG(flood_judge_off, "flood_judge_off", "dev", false, "VOTVCOOP_FLOOD_JUDGE_OFF",
    "Developer red arm of the flood drill (host): never set. The host applies a client's repair of a pipe with no check of the "
    "sender's life, reach or toolbox, so far must say FAIL: its raw ask mends the pipe.")
CFG_FLAG(flood_join_off, "flood_join_off", "dev", false, "VOTVCOOP_FLOOD_JOIN_OFF",
    "Developer red arm of the flood drill (host): never set. The host sends a joiner no pipe and no level at its ready edge, so "
    "join must say FAIL: the joiner's pipes read whole.")
