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
CFG_FLAG(cremator_drill, "cremator_drill", "dev", false, "VOTVCOOP_CREMATOR_DRILL",
    "Drill (BOTH peers): the cremator's lever run crosses. The host pulls the lever on its copy and the client's copy "
    "must read it on, its door latched, and then off again (G1); with the rig's --rejoin the host pulls it on once "
    "more and the client, leaving and coming back, must read it on after its world-ready (G4). The host prints "
    "'cremator-drill: VERDICT PASS|FAIL <arm>' from what the client signalled; G1 ends at 'cremator-drill: G1 DONE', "
    "the rejoin run at 'cremator-drill: DONE on host'. The second life carries VOTVCOOP_CREMATOR_DRILL_REJOIN=1. Red "
    "arm: the host's cremator_state_off. It MUTATES the world.")
CFG_FLAG(cremator_hit_off, "cremator_hit_off", "dev", false, "VOTVCOOP_CREMATOR_HIT_OFF",
    "Developer red arm of the cremator burn drill: never set. The host sends no hit for a cremator's damage or ignite "
    "on a client's puppet (the puppet still takes none), so a client standing in the host's running cremator keeps "
    "its health.")
