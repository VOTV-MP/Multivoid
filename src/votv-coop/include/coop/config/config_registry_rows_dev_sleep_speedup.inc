// coop/config/config_registry_rows_dev_sleep_speedup.inc -- the [dev] rows of the sleep speed-up setting: its drill and
// its red arm. Included by config_registry_rows_dev.inc after the nightmare rows, so each row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(sleep_speedup_drill, "sleep_speedup_drill", "dev", false, "VOTVCOOP_SLEEP_SPEEDUP_DRILL",
    "Drill, whose sleep speeds the world up; set on BOTH peers, with the peers' folder VOTVCOOP_CAN_DIR, needs left "
    "alone (VOTVCOOP_FILL_NEEDS=0) and a fresh host world. One arm per value of the host's world.sleep_speedup (the "
    "rig's --host-config). The host sleeps with the client awake and reads the world's dilation at each edge; the "
    "host says 'rig: VERDICT sleep-speedup PASS|FAIL|INVALID -- <mode>: <what>', and each peer 'DONE'. everyone: the "
    "world reads 1 while the client is awake, the game's factor once it sleeps, and 1 once it wakes. host: the game's "
    "factor at once. never: 1 with both asleep. It MUTATES the world (dev writes: the needs, the host's nightmare "
    "chance).")
CFG_FLAG(sleep_speedup_rule_off, "sleep_speedup_rule_off", "dev", false, "VOTVCOOP_SLEEP_SPEEDUP_RULE_OFF",
    "Developer red arm of the sleep speed-up drill: never set. The host ignores world.sleep_speedup and acts as host, "
    "so its sleep speeds the world up whoever is awake: the everyone and never arms must FAIL at the host's first "
    "read.")
