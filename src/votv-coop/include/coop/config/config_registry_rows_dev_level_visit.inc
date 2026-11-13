// coop/config/config_registry_rows_dev_level_visit.inc -- the [dev] rows of the level-visit lane: its red arms. Included by
// config_registry_rows_dev.inc after the nightmare rows, so each row keeps its place in every expansion of the list.
// The entry forms are config_registry_rows.inc's.

CFG_FLAG(level_visit_hide_off, "level_visit_hide_off", "dev", false, "VOTVCOOP_LEVEL_VISIT_HIDE_OFF",
    "Developer red arm of the level-visit drill: never set. The copy of a player in a visited map is not hidden on this "
    "machine, so it stays where its pose puts it, far below the world.")
CFG_FLAG(level_visit_copy_cancel_off, "level_visit_copy_cancel_off", "dev", false, "VOTVCOOP_LEVEL_VISIT_COPY_CANCEL_OFF",
    "Developer red arm of the level-visit drill: never set. A trip to one of the backrooms' maps authored by a peer's "
    "copy on this machine is let run instead of cancelled, so this machine travels with it.")
CFG_FLAG(backrooms_close_off, "backrooms_close_off", "dev", false, "VOTVCOOP_BACKROOMS_CLOSE_OFF",
    "Developer red arm of the level-visit drill: never set. The game's own clip check is not answered by the mod on "
    "this machine, so a clip is taken while the host has the backrooms closed, and during a visit.")
