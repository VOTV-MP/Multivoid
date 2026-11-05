// coop/config/config_registry_rows_dev_cremator.inc -- the [dev] rows of the cremator probe and the lever lane.
// Included by config_registry_rows_dev.inc after its last row, as _dev_vulkan.inc is, so the rows keep their place in
// every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(cremator_probe, "cremator_probe", "dev", "off", "off|1|host|placeonly", "VOTVCOOP_CREMATOR_PROBE",
    "Probe (a cremator's own burn on a coop player). 1 -- the CLIENT walks into the nearest cremator's firebox, "
    "shuts the door and pulls the lever on its own copy. host -- the HOST opens the nearest cremator's door, "
    "sends the client's puppet to the firebox with the admin verb's teleport order, and once the cremator's own "
    "begin-overlap sees the puppet shuts the door and pulls the lever on its own copy. placeonly -- the CLIENT "
    "moves nothing: it waits for the host's move and reports its health (pair it with host on the host). One "
    "line a second; lines tagged 'cremator-probe:'; "
    "the DONE line ends it ('HOST DONE' on the host). It MUTATES the world.")
CFG_FLAG(cremator_state_off, "cremator_state_off", "dev", false, "VOTVCOOP_CREMATOR_STATE_OFF",
    "Developer red arm of the cremator lever drill: never set. The host reports no cremator state, so its lever run "
    "sends no edge and no connect snapshot and the client's copy stays idle.")
CFG_FLAG(cremator_intent_anim_off, "cremator_intent_anim_off", "dev", false, "VOTVCOOP_CREMATOR_INTENT_ANIM_OFF",
    "Developer red arm of the cremator lever drill: never set. The host's judge of a client's handle press skips its "
    "check that its own handle is at rest, so two presses close together both pull the lever.")
CFG_FLAG(cremator_intent_reach_off, "cremator_intent_reach_off", "dev", false, "VOTVCOOP_CREMATOR_INTENT_REACH_OFF",
    "Developer red arm of the cremator lever drill: never set. The host's judge of a client's handle press skips its "
    "reach check, so a press from beyond the handle's reach pulls the lever.")
