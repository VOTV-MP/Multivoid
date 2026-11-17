// coop/config/config_registry_rows_dev_sleep.inc -- the [dev] rows of the sleep lane: the sleep drill, and the sleep
// speed-up setting's drill and red arm. Included by config_registry_rows_dev.inc where the sleep drill stood, so each
// row keeps its place in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(sleep_drill, "sleep_drill", "dev", "off", "off|host_asleep|client_rest|beside", "VOTVCOOP_SLEEP_DRILL",
    "Drill (BOTH peers), once a client's join is over: what a host's sleep and a client's rest do. "
    "host_asleep: the host sets its need to 80 and sleeps natively and says the world's dilation it "
    "reads (20); the client reads its own world's dilation (1) and its clock for 10 s, and the clock "
    "must run at 0.9 of the native rate. client_rest: the host sleeps and prints its real seconds; "
    "when it wakes the client reads its food for 10 s, sleeps from a need of 80 in the nearest bed "
    "and prints its real seconds, its world's dilation, and its food and need at both ends. The host "
    "holds its nightmare chance at 0 for the arm. beside: the host sleeps and is held asleep (its need is "
    "written to 50 every tick) until the hook drag selftest has ended on it, then wakes; run it with "
    "hookdrag_selftest=1 and hookdrag_role=client on both peers. The client prints its armed line only. "
    "Every phase ends on a verdict line "
    "([SLEEP-DRILL] <arm> <role> PASS|FAIL), and a phase that does not come is a FAIL after a bound.")
CFG_ENUM(sleep_speedup_drill, "sleep_speedup_drill", "dev", "off", "off|modes|row_change|leave",
    "VOTVCOOP_SLEEP_SPEEDUP_DRILL",
    "Drill, whose sleep speeds the world up; set on BOTH peers, with the peers' folder VOTVCOOP_CAN_DIR, needs left "
    "alone (VOTVCOOP_FILL_NEEDS=0) and a fresh host world. The host sleeps with the client awake and reads the "
    "world's dilation at each edge; the host says 'rig: VERDICT sleep-speedup PASS|FAIL|INVALID -- <arm>: <what>', and "
    "each peer 'DONE'. modes: one arm per value of the host's world.sleep_speedup (the rig's --host-config). everyone: "
    "the world reads 1 while the client is awake, the game's factor once it sleeps, and 1 once it wakes. host: the "
    "game's factor at once. never: 1 with both asleep. row_change (the host's row everyone): the host asleep and the "
    "client awake reads 1, the row set to host for the session reads the game's factor at once, set back to everyone "
    "it reads 1. leave (the host's row everyone or never): both asleep, the client leaves: everyone reads the game's "
    "factor, never still reads 1. A dead sleep or wakeup watch is INVALID. It MUTATES the world (dev writes: the "
    "needs, the host's nightmare chance, the row's session value).")
CFG_FLAG(sleep_speedup_rule_off, "sleep_speedup_rule_off", "dev", false, "VOTVCOOP_SLEEP_SPEEDUP_RULE_OFF",
    "Developer red arm of the sleep speed-up drill: never set. The host ignores world.sleep_speedup and acts as host, "
    "so its sleep speeds the world up whoever is awake: the everyone and never arms must FAIL at the host's first "
    "read.")
