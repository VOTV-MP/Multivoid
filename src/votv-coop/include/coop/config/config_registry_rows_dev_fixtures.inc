// coop/config/config_registry_rows_dev_fixtures.inc -- the [dev] rows of the drills for a fixture a player works
// on. Included by config_registry_rows_dev.inc, so its rows join every expansion of the list. The entry forms are config_registry_rows.inc's.
CFG_ENUM(oven_progress_drill, "oven_progress_drill", "dev", "off", "off|host|client|join",
    "VOTVCOOP_OVEN_PROGRESS_DRILL",
    "Drill (BOTH peers): the oven's screws and lid. host and client: the acting peer sets a screw's "
    "bool on its own repair screen, then the other three, then the lid's; the watching peer says its "
    "screen's mask whenever it changes and each step must stand there before the next. join: the "
    "host's screen takes all five before anyone joins and the joiner judges its own screen when the "
    "host's mask reaches it. One `[OVEN-DRILL] [H|C] PASS` or `FAIL` line per judge, each decided "
    "when a mask arrives; a wait that never ends says `INVALID`. An oven the save holds repaired "
    "ends INVALID by name.")
CFG_FLAG(oven_merge_off, "oven_merge_off", "dev", false, "VOTVCOOP_OVEN_MERGE_OFF",
    "Developer red arm of the oven drill: never set. This peer counts a repair-step mask that reaches it "
    "with a bit its screen lacks and does not raise the bit, so the drill's judge on this peer must FAIL at "
    "that arrival.")
CFG_ENUM(pried_drill, "pried_drill", "dev", "off",
    "off|plank_host|plank_client|radiator_host|radiator_client|measure_plank|measure_radiator",
    "VOTVCOOP_PRIED_DRILL",
    "Drill (BOTH peers): a crowbar's pry of a prop with no wall-attach component, by the prop's own "
    "crowbarOpen with no crowbar. The watching peer names the first stuck prop of the class (a plank, a "
    "radiator), the acting peer pries it and says where its copy rests, and the watching peer judges its "
    "own copy. One `[PRIED-DRILL] [H|C] PASS` or `FAIL` line per judge. measure_plank and measure_radiator "
    "judge nothing: the host names the prop, the client pries it, and each peer logs `[PRY-MEASURE]` lines "
    "(every crowbarOpen, a radiator's breakingSequence and BreakConstraint, each part's pose every 500 ms "
    "until 5 s after the last break, the rest poses). Run with the rig's --rejoin, --rejoin-marker "
    "'pried-measure: REJOIN MARKER' and --rejoin-env VOTVCOOP_PRIED_MEASURE_REJOIN=1: the rejoined client "
    "logs what it sees of the same prop, and the host ends on 'rig: VERDICT pry-measure DONE' (INVALID when "
    "a wait ran out). Run it on a fresh host world.")
