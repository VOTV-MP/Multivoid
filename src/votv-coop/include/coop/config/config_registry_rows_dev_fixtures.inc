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
    "fails by name.")
CFG_FLAG(oven_merge_off, "oven_merge_off", "dev", false, "VOTVCOOP_OVEN_MERGE_OFF",
    "Developer red arm of the oven drill: never set. This peer counts a repair-step mask that reaches it "
    "with a bit its screen lacks and does not raise the bit, so the drill's judge on this peer must FAIL at "
    "that arrival.")
