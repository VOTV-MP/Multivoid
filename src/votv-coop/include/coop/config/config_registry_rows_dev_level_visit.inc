// coop/config/config_registry_rows_dev_level_visit.inc -- the [dev] rows of the level-visit lane: its drill, the shape of
// the visit it takes and its red arms. Included by config_registry_rows_dev.inc after the nightmare rows, so each row
// keeps its place in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(level_visit_drill, "level_visit_drill", "dev", "off", "off|client|host|close|join", "VOTVCOOP_RUN_LEVEL_VISIT",
    "Drill, the level-visit lane (a clip's trip to another map is the clipping player's alone); set on BOTH peers, with "
    "the peers' folder VOTVCOOP_CAN_DIR. client -- the client is the visitor and the host the watcher; host -- the roles "
    "swapped. The visitor takes the trip (level_visit_start), stays until the watcher has judged its copy of the "
    "visitor hidden by the poses it applies, ends the visit (level_visit_end) and comes back; the watcher judges the "
    "copy hidden at the first pose below the visit floor and shown at the first pose back above it. close -- the host "
    "closes the backrooms: the client's clip is answered no, and after the host opens them again a clip takes a trip. "
    "join -- "
    "the host takes the trip and stays; the client, launched to join later, must find the host's copy hidden at the "
    "first pose it applies (INVALID when the host was not yet in the map). The host prints 'rig: VERDICT level-visit "
    "PASS|FAIL|INVALID' and 'level-visit: DONE <role>'; each peer prints 'level-visit: ARM <step> <role> "
    "PASS|FAIL|INVALID'. Red arms: level_visit_hide_off and backrooms_close_off. It "
    "MUTATES the world (the visit loads a map, the host's setting changes).")
CFG_ENUM(level_visit_start, "level_visit_start", "dev", "clip", "clip|call|none", "VOTVCOOP_LEVEL_VISIT_START",
    "How the level-visit drill's visitor takes the trip: clip -- the game's own clip (lastLoc under the floor, no camera "
    "taken, the clip's own map pick, which level_visit_map does not change); call -- the clip's own lib_C::loadLevel for "
    "level_visit_map, with a camera the host spawned taken in hand first; none -- the control: no visit, a lethal hit in "
    "the world, and the usual revive.")
CFG_ENUM(level_visit_look, "level_visit_look", "dev", "off", "off|lamp", "VOTVCOOP_LEVEL_VISIT_LOOK",
    "Level-visit drill: lamp -- the visitor turns to the map's lantern, takes it in hand, lights it with its own RMB use "
    "and puts it down before the end, taking no camera.")
CFG_STRING(level_visit_map, "level_visit_map", "dev", "", "VOTVCOOP_LEVEL_VISIT_MAP", false,
    "Level-visit drill, level_visit_start=call: the map the trip asks for (empty = sl_backroomsObsolete).")
CFG_ENUM(level_visit_end, "level_visit_end", "dev", "end", "end|quit|wait|death", "VOTVCOOP_LEVEL_VISIT_END",
    "How the level-visit drill's visitor ends the visit: end -- lib_C::End called from the map's level script; quit -- "
    "QuitGame with it; wait -- the map's own clock (the backrooms' ten minutes); death -- a lethal hit through the "
    "game's own Add Player Damage.")
CFG_STRING(level_visit_dump_dir, "level_visit_dump_dir", "dev", "", "VOTVCOOP_LEVEL_VISIT_DUMP_DIR", false,
    "Level-visit drill: a folder the visitor writes the visited map's own Blueprint classes into, as script text, once "
    "it is in the map. Empty = none.")

CFG_FLAG(level_visit_hide_off, "level_visit_hide_off", "dev", false, "VOTVCOOP_LEVEL_VISIT_HIDE_OFF",
    "Developer red arm of the level-visit drill: never set. The copy of a player in a visited map is not hidden on this "
    "machine, so it stays where its pose puts it, far below the world.")
CFG_FLAG(backrooms_close_off, "backrooms_close_off", "dev", false, "VOTVCOOP_BACKROOMS_CLOSE_OFF",
    "Developer red arm of the level-visit drill: never set. The game's own clip check is not answered by the mod on "
    "this machine, so a clip is taken while the host has the backrooms closed, and during a visit.")
