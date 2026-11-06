// coop/config/config_registry_rows_dev_director.inc -- the [dev] rows of the bot director. Included by
// config_registry_rows_dev.inc after its last row, as _dev_cremator.inc is, so the rows keep their place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(director_clearway_off, "director_clearway_off", "dev", false, "VOTVCOOP_DIRECTOR_CLEARWAY_OFF",
    "Developer red arm of the director's clearway: never set. A walk stopped by a physics prop on its route no longer "
    "picks the prop up and sets it aside; the prop is left to the way-round search, which finds none where the prop "
    "stands in a room's only way in.")
