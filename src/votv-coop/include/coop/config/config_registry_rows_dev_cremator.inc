// coop/config/config_registry_rows_dev_cremator.inc -- the [dev] row of the cremator probe. Included by
// config_registry_rows_dev.inc after its last row, as _dev_vulkan.inc is, so the row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(cremator_probe, "cremator_probe", "dev", "off", "off|1|host|placeonly", "VOTVCOOP_CREMATOR_PROBE",
    "Probe (a cremator's own burn on a coop player). 1 -- the CLIENT stands in the nearest cremator's firebox, "
    "shuts the door and pulls the lever on its own copy. host -- the HOST runs the lever on its own copy once the "
    "client's puppet has stood in a firebox for two checks. placeonly -- the CLIENT only stands in the firebox "
    "and reports its health (pair it with host on the host). One line a second; lines tagged 'cremator-probe:'; "
    "the DONE line ends it ('HOST DONE' on the host). It MUTATES the world.")
