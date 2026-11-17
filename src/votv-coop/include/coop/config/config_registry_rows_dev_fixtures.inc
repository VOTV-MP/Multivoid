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