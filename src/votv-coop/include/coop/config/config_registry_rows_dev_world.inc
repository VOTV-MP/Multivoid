// coop/config/config_registry_rows_dev_world.inc -- the [dev] rows of the world drills: the day's rollover and sleep, the Bad
// Sun, the creatures' event relay and hit, and the day's world rolls. Included by config_registry_rows_dev.inc after the
// rollover watch, so each row keeps its place in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(midnight_drill, "midnight_drill", "dev", "off", "off|awake|asleep|cheat|mode5|malformed|joinwindow",
    "VOTVCOOP_MIDNIGHT_DRILL",
    "Drill (BOTH peers, with rollover_watch), once a client's join is over. awake and asleep: the "
    "host sets its clock just short of midnight; awake: both peers stay up; asleep: the host goes to "
    "bed and the client follows once it sees the host asleep, so the midnight falls inside the host's "
    "own fast-forward (each peer sleeps on its own). cheat: the host leaves its "
    "clock alone and the client writes a day onto its own, three times, as the cheat menu's day "
    "button does. mode5: each peer spawns game mode 5's master, which resets the day every second, "
    "and the client says whether its own reset kept running. malformed: the host puts three clock "
    "samples whose day is not a number, and the client's log says where each was refused. joinwindow: "
    "the host sets its clock just short of midnight once it has taken a joiner's world, so the midnight "
    "falls inside the join, and both peers print their dishes' hash digest; equal is a pass. Run it on a "
    "fresh host world; every refused step is logged and ends the arm as invalid.")
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
CFG_ENUM(badsun_drill, "badsun_drill", "dev", "off", "off|clock|midnight|spawn|cheat|join|end|absent_stale", "VOTVCOOP_BADSUN_DRILL",
    "Drill (BOTH peers), the Bad Sun: does a peer's own loaded copy go "
    "active by its own clock. clock, join and end need the save that holds one saved Bad Sun, the slot s_rig_badsun "
    "(VOTVCOOP_RIG_SLOT=s_rig_badsun: the everyday rig slot had it removed by tools/rig_strip_class.py); the others a fresh host world. clock: the host parks the clock at 05:00 at hosting and sets 06:30 once a "
    "client's join is over; each peer reads its count of live copies (it must be 1), its copy's widget "
    "and its place in the active events' senders, and its own clock every second, from before 07:00; "
    "the copy must stay inactive until the clock reads 07:00 and be active by both tests within 15 s "
    "after it. Each peer ends on a verdict line ([BADSUN-DRILL] clock <role> PASS|FAIL|INCONCLUSIVE; "
    "INCONCLUSIVE: the save's copy was active at hosting, or the peer did not see its clock cross 07:00). "
    "midnight, spawn, cheat, join, end and absent_stale: whether the host's existence of a Bad Sun reaches "
    "each peer's own copies through the lane (the day edge's in-graph spawn, the host's call, a client's own refused "
    "call, a joiner's seed, the end by the host's clock, a stale copy in a joiner's blob); each arm's set-up "
    "and verdicts are in src/coop/dev/badsun_drill_shared.h, and each ends on its verdict line.")
CFG_ENUM(event_relay_drill, "event_relay_drill", "dev", "off", "off|on", "VOTVCOOP_EVENT_RELAY_DRILL",
    "Drill (BOTH peers, one run, one rejoin), the host's creature events in a client's event count: the host "
    "spawns an inert krampus 15 m above itself once the client's join is over; the client reads its mirror's "
    "copies in the gamemode's event senders (it must be 1, the relay's one), its count and the game's getEvent, "
    "tells the host by chat, and rejoins and reads the same; the host then destroys the krampus and the client's "
    "count must fall by one while the host's returns to where it began. Each step ends on a [EVR] PASS or "
    "[EVR] FAIL line (src/coop/dev/event_relay_drill.cpp); a step that does not come is a FAIL after 60 s.")
CFG_ENUM(creature_hit_drill, "creature_hit_drill", "dev", "off", "off|zombie|fossilhound", "VOTVCOOP_CREATURE_HIT_DRILL",
    "Drill (BOTH peers, one run), a parked creature's hit and a client's hit on it: the host spawns the creature "
    "300 cm in front of the client's puppet once the client's join is over; its hits on the puppet must reach the "
    "client three times and none may come from the mirror's own copy; then the client, with an empty hand and "
    "then with an axe, calls the mirror's addDamage, and the host must refuse the first and judge the rest until "
    "its creature dies and the mirror is destroyed. The fossilhound arm adds the mirror's own per-viewer event. "
    "Each step ends on a [CHD] PASS or [CHD] FAIL line (src/coop/dev/creature_hit_drill.cpp); a step that does "
    "not come is a FAIL after 60 s.")
CFG_ENUM(world_roll_drill, "world_roll_drill", "dev", "off",
    "off|eye_host|eye_client|eye_join|jelly_host|jelly_client|jelly_join|redsky_host|redsky_client|redsky_join",
    "VOTVCOOP_WORLD_ROLL_DRILL",
    "Drill (BOTH peers), the day's world rolls: each a host roll that must cross and a client's own that "
    "must be refused. eye_host: once a client's join is over the host's sky runs setEye(true), as its noon "
    "roll would, and the client's copy must show the eye within 20 s; eye_client: the joined client runs "
    "its own setEye(true) and the gate must refuse it, its copy's eye unchanged; eye_join: the host's sky "
    "runs setEye(true) before any client's world is ready, and the joiner's copy must show it. jelly_host: "
    "once a client's join is over the host's jellyfish path spawns, as its 18:00 roll would, and the "
    "client's copy must show the seven fish as mirrors within 20 s and see them leave with the host's run; "
    "jelly_client: the joined client's own spawn must leave its path's list as it was; jelly_join: the "
    "host's run is under way before any client's world is ready. redsky_host: once a client's join is over "
    "the host's gamemode runs its noon toggle twice, starting a red sky and ending it, and the client's "
    "copy must see both edges within 20 s; redsky_client: the joined client's own toggle must be refused, "
    "its sky unchanged; redsky_join: the host's red sky stands before any client's world is ready, the "
    "joiner's copy must show it within 20 s, and then the joiner's own toggle, as its noon would run it with "
    "a red sky live, must be refused, its sky still red. Run it on a fresh host world; every refused step is "
    "logged and ends the arm as invalid.")
