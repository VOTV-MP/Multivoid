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
