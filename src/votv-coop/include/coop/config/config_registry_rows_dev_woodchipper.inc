// coop/config/config_registry_rows_dev_woodchipper.inc -- the [dev] rows of the woodchipper lane: its drill and its red
// arms. Included by config_registry_rows_dev.inc after the cremator rows, so each row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(woodchipper_gate_off, "woodchipper_gate_off", "dev", false, "VOTVCOOP_WOODCHIPPER_GATE_OFF",
    "Developer red arm of the woodchipper drill: never set. A client's chipper copy is not refused its tick and its "
    "intake, so it burns and eats on its own beside the host's.")
CFG_FLAG(woodchipper_intent_off, "woodchipper_intent_off", "dev", false, "VOTVCOOP_WOODCHIPPER_INTENT_OFF",
    "Developer red arm of the woodchipper drill: never set. A client's pour and press at a chipper are not sent to "
    "the host, so nothing it does there crosses.")
CFG_FLAG(woodchipper_pour_check_off, "woodchipper_pour_check_off", "dev", false, "VOTVCOOP_WOODCHIPPER_POUR_CHECK_OFF",
    "Developer red arm of the woodchipper drill: never set. The host's judge of a client's pour skips its checks of the "
    "gas can in the sender's hand and of the room in the tank, so a pour from an empty hand is taken and an over-pour "
    "fills the chipper past its tank.")
CFG_FLAG(woodchipper_replay_off, "woodchipper_replay_off", "dev", false, "VOTVCOOP_WOODCHIPPER_REPLAY_OFF",
    "Developer red arm of the woodchipper drill: never set. The host does not tell a joiner which chippers are on at "
    "its ready edge, so a rejoined client's copy of a running chipper reads it off.")
