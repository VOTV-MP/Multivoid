// coop/config/config_registry_rows_dev_level_visit.inc -- the [dev] rows of the level-visit lane: its red arms. Included by
// config_registry_rows_dev.inc after the nightmare rows, so each row keeps its place in every expansion of the list.
// The entry forms are config_registry_rows.inc's.

CFG_FLAG(level_visit_hide_off, "level_visit_hide_off", "dev", false, "VOTVCOOP_LEVEL_VISIT_HIDE_OFF",
    "Developer red arm of the level-visit drill: never set. The copy of a player in a visited map is not hidden on this "
    "machine, so it stays where its pose puts it, far below the world.")
