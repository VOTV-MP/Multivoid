// coop/config/config_registry_rows_dev_prop_props.inc -- the [dev] rows of the prop-properties tool's lane: its red
// arms. Included by config_registry_rows_dev.inc after the woodchipper rows, so each row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(prop_props_gate_off, "prop_props_gate_off", "dev", false, "VOTVCOOP_PROP_PROPS_GATE_OFF",
    "Developer red arm of the prop-properties drill: never set. A client's press of the toolgun's prop-properties tool "
    "is not cancelled and not sent to the host, so it runs on the client's copy alone and the host's copy keeps its "
    "flags.")
CFG_FLAG(prop_props_judge_off, "prop_props_judge_off", "dev", false, "VOTVCOOP_PROP_PROPS_JUDGE_OFF",
    "Developer red arm of the prop-properties drill: never set. The host's judge of a client's press skips its check of "
    "the player's right to the tool, so a press from a player the host has withheld it from is applied.")
CFG_FLAG(prop_props_park_selftest, "prop_props_park_selftest", "dev", false, "VOTVCOOP_PROP_PROPS_PARK_SELFTEST",
    "Selftest (either peer, once per process, in the first tick): the order in which the prop-properties tool's states "
    "wait for a held copy and replay, on stated inputs -- a newer state that reached the copy directly is never "
    "overwritten by the older one parked for it, the newest parked state wins, a gone copy is forgotten, a copy held past "
    "the warning keeps its state for the hold's end. Prints \"prop_tool_park selftest: ALL PASS\" or FAIL.")
CFG_FLAG(prop_props_park_selftest_break, "prop_props_park_selftest_break", "dev", false, "VOTVCOOP_PROP_PROPS_PARK_SELFTEST_BREAK",
    "Red arm of the park selftest: never set. The first case does not supersede the older parked state, so the "
    "replay applies it over the newer press and the selftest must print FAIL.")
