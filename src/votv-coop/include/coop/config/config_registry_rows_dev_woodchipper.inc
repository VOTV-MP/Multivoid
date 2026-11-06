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
CFG_FLAG(woodchipper_drill, "woodchipper_drill", "dev", false, "VOTVCOOP_WOODCHIPPER_DRILL",
    "Drill, the woodchipper lane; set on BOTH peers, with the rig's --rejoin for the last arm. The host finds or spawns a "
    "woodchipper beside itself; the client's player is walked to the host by the director, takes a gas can into its hand "
    "and pours it, presses the button, sends an over-pour and an empty-handed pour, and a paper dropped over the hopper "
    "is chipped. The host prints 'rig: VERDICT woodchipper PASS|FAIL|INVALID' (arms g1, g2, g3, g5, g6, one line "
    "'woodchipper: ARM <g> PASS|FAIL' each; g3 says INVALID when the host never eats the dropped prop) and "
    "'woodchipper: G1-G6 DONE'; the rejoined client's copy must read the chipper on after its world-ready: 'rig: VERDICT "
    "woodchipper-rejoin PASS|FAIL' and 'woodchipper: DONE on host'. The second life carries "
    "VOTVCOOP_WOODCHIPPER_DRILL_REJOIN=1. Red arms: the client's woodchipper_gate_off and woodchipper_intent_off (g2, and "
    "g1 for the second), the host's woodchipper_pour_check_off (g5, g6) and woodchipper_replay_off (the rejoin). It "
    "MUTATES the world. Run it on a fresh host world.")
