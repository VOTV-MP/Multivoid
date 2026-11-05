// coop/config/config_registry_rows_dev_cremator.inc -- the [dev] row of the cremator probe. Included by
// config_registry_rows_dev.inc after its last row, as _dev_vulkan.inc is, so the row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(cremator_probe, "cremator_probe", "dev", false, "VOTVCOOP_CREMATOR_PROBE",
    "Probe (the CLIENT): the local player stands in the nearest cremator's firebox, the door is shut and the "
    "lever is pulled on the client's own copy; one line a second says what the cremator's own burn does to the "
    "player and what the death flow does after. Lines tagged 'cremator-probe:'; its DONE line ends it. It MUTATES "
    "the world.")
