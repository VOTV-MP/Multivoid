// coop/config/config_registry_rows_dev_cremator.inc -- the [dev] row of the cremator probe. Included by
// config_registry_rows_dev.inc after its last row, as _dev_vulkan.inc is, so the row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(cremator_probe, "cremator_probe", "dev", "off", "off|1|host|placeonly", "VOTVCOOP_CREMATOR_PROBE",
    "Probe (a cremator's own burn on a coop player). 1 -- the CLIENT walks into the nearest cremator's firebox, "
    "shuts the door and pulls the lever on its own copy. host -- the HOST opens the nearest cremator's door, "
    "sends the client's puppet to the firebox with the admin verb's teleport order, and once the cremator's own "
    "begin-overlap sees the puppet shuts the door and pulls the lever on its own copy. placeonly -- the CLIENT "
    "moves nothing: it waits for the host's move and reports its health (pair it with host on the host). One "
    "line a second; lines tagged 'cremator-probe:'; "
    "the DONE line ends it ('HOST DONE' on the host). It MUTATES the world.")
