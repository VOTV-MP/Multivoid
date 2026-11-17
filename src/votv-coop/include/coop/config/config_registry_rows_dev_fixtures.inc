// coop/config/config_registry_rows_dev_fixtures.inc -- the [dev] rows of the drills for a fixture a player works
// on. Included by config_registry_rows_dev.inc where these rows stood, so each keeps its place in
// every expansion of the list. The entry forms are config_registry_rows.inc's.
CFG_ENUM(oven_progress_drill, "oven_progress_drill", "dev", "off", "off|host|client|join",
    "VOTVCOOP_OVEN_PROGRESS_DRILL",
    "Drill (BOTH peers): the oven's screws and lid. host and client: the acting peer sets a screw's "
    "bool on its own repair screen, then the other three, then the lid's; the watching peer says its "
    "screen's mask whenever it changes and each step must stand there before the next. join: the "
    "host's screen takes all five before anyone joins and the joiner judges its own screen after its "
    "join. One `[OVEN-DRILL] [H|C] PASS` or `FAIL` line per judge; an oven the save holds repaired "
    "fails by name.")
