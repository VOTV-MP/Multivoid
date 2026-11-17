// coop/config/config_registry_rows_dev_fixtures.inc -- the [dev] rows of the drills for a fixture a player works
// on with a tool. Included by config_registry_rows_dev.inc where these rows stood, so each keeps its place in
// every expansion of the list. The entry forms are config_registry_rows.inc's.
CFG_ENUM(pried_drill, "pried_drill", "dev", "off",
    "off|plank_host|plank_client|radiator_host|radiator_client",
    "VOTVCOOP_PRIED_DRILL",
    "Drill (BOTH peers): a crowbar's pry of a prop with no wall-attach component, by the prop's own "
    "crowbarOpen with no crowbar. The watching peer names the first stuck prop of the class (a plank, a "
    "radiator), the acting peer pries it and says where its copy rests, and the watching peer judges its "
    "own copy. One `[PRIED-DRILL] [H|C] PASS` or `FAIL` line per judge.")
CFG_ENUM(oven_progress_drill, "oven_progress_drill", "dev", "off", "off|host|client|join",
    "VOTVCOOP_OVEN_PROGRESS_DRILL",
    "Drill (BOTH peers): the oven's screws and lid. host and client: the acting peer sets a screw's "
    "bool on its own repair screen, then the other three, then the lid's; the watching peer says its "
    "screen's mask whenever it changes and each step must stand there before the next. join: the "
    "host's screen takes all five before anyone joins and the joiner judges its own screen after its "
    "join. One `[OVEN-DRILL] [H|C] PASS` or `FAIL` line per judge; an oven the save holds repaired "
    "fails by name.")
CFG_ENUM(desk_panel_drill, "desk_panel_drill", "dev", "off", "off|host|client|race",
    "VOTVCOOP_DESK_PANEL_DRILL",
    "Drill (BOTH peers): the desk's back panels. host and client: the acting peer unbolts the top-left "
    "panel through the desk's own unscrewPanel, takes it off through the desk's own E press, and "
    "puts the panel prop back into its place; after each step the watching peer says the masks its "
    "desk holds, and the acting peer judges them against its own. race: both peers take the same "
    "unbolted panel on one net-clock instant, and each judges that one panel prop is left. One "
    "`[DESK-PANEL-DRILL] [H|C] PASS` or `FAIL` line per judge.")
