// coop/config/config_registry_rows_dev_grime.inc -- the [dev] rows of the grime lane's drills (coop/interactables/grime_sync,
// coop/world/spill_sync): the rehost churn's red arm. Included by config_registry_rows_dev.inc at its end, so each row keeps its
// place in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(grime_replay_watch_off, "grime_replay_watch_off", "dev", false, "VOTVCOOP_GRIME_REPLAY_WATCH_OFF",
    "Developer red arm of the rehost churn (VOTVCOOP_RUN_RELOAD_CHURN=1 VOTVCOOP_RELOAD_HOST=1 VOTVCOOP_RELOAD_CYCLES=2): "
    "never set; the host. The grime lane does not forget its index as the save replay begins, so the replay's deletes of "
    "the level's own decals read as wipes to zero and the save's decals at those places are lowered: the drill must say "
    "'rig: VERDICT reload-churn FAIL' with the count of decals lowered.")
