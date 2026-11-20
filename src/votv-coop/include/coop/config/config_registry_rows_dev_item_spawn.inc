// coop/config/config_registry_rows_dev_item_spawn.inc -- the [dev] rows of the item-spawn drill (its arm and its red).
// Included by config_registry_rows.inc after the dev rows. The entry forms are config_registry_rows.inc's.

CFG_FLAG(item_spawn_drill, "item_spawn_drill", "dev", false, "VOTVCOOP_ITEM_SPAWN_DRILL",
    "Drill (the host; the client does nothing): an item a refund or a hand-over makes is spawned with its class loaded "
    "first. For each spawn site whose item class this world has not loaded (the power upgrade, the coordination tower's "
    "fuse, the server upgrade, the mining rig's GPU and cooler, the ATV battery) the host spawns the item in front of "
    "itself and it must stand. It prints 'rig: VERDICT item-spawn PASS|FAIL|INVALID' (INVALID when every class is "
    "loaded already) and 'item-spawn: DONE on host'. It MUTATES the world (a few props). Run it on a fresh world.")
CFG_FLAG(item_spawn_no_load, "item_spawn_no_load", "dev", false, "VOTVCOOP_ITEM_SPAWN_NO_LOAD",
    "Developer red arm of the item-spawn drill: never set. The class resolver's registry step is off, so a class the "
    "host has not loaded is not loaded and the item is lost, as with a lookup in the object index alone; the verdict "
    "is FAIL.")
