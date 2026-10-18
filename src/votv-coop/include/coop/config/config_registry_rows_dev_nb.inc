// coop/config/config_registry_rows_dev_nb.inc -- the [dev] rows of the NPC-spawner arc (NB): the native-spawn
// probe. Included by config_registry_rows_dev.inc after its last row, as _dev_pi.inc is, so each row keeps its
// place in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(native_spawn_probe, "native_spawn_probe", "dev", "off", "off|record|orb", "VOTVCOOP_NATIVE_SPAWN_PROBE",
    "Dev probe (BOTH peers): every allowlisted creature this machine spawns from a Blueprint graph, one [NB-0] "
    "line per new source; orb: a client also asks its blackball spawners to spawn.")
