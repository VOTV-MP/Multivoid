// coop/config/config_registry_rows_dev_woodchipper.inc -- the [dev] rows of the woodchipper lane: its drill and its red
// arms. Included by config_registry_rows_dev.inc after the cremator rows, so each row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(woodchipper_gate_off, "woodchipper_gate_off", "dev", false, "VOTVCOOP_WOODCHIPPER_GATE_OFF",
    "Developer red arm of the woodchipper drill: never set. A client's chipper copy is not refused its tick and its "
    "intake, so it burns and eats on its own beside the host's.")
CFG_FLAG(woodchipper_intent_off, "woodchipper_intent_off", "dev", false, "VOTVCOOP_WOODCHIPPER_INTENT_OFF",
    "Developer red arm of the woodchipper drill: never set. A client's pour and press at a chipper are not sent to "
    "the host, so nothing it does there crosses.")
