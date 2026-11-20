// coop/config/config_registry_rows_dev_woodchipper.inc -- the [dev] rows of the woodchipper lane: its drill and its red
// arms. Included by config_registry_rows_dev.inc after the cremator rows, so each row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(woodchipper_gate_off, "woodchipper_gate_off", "dev", false, "VOTVCOOP_WOODCHIPPER_GATE_OFF",
    "Developer red arm of the woodchipper drill: never set. A client's chipper copy is not refused its tick and its "
    "intake, so it burns and eats on its own beside the host's.")
CFG_FLAG(woodchipper_intent_off, "woodchipper_intent_off", "dev", false, "VOTVCOOP_WOODCHIPPER_INTENT_OFF",
    "Developer red arm of the woodchipper drill: never set. A client's pour and press at a chipper are not sent to "
    "the host, so nothing it does there crosses.")
CFG_FLAG(chipper_driven_flip_off, "chipper_driven_flip_off", "dev", false, "VOTVCOOP_CHIPPER_DRIVEN_FLIP_OFF",
    "Developer red arm of the chipper feed drill: set on the HOST, never in play. The host's chipper does not set a prop "
    "a client carries or throws into its box simulating for its intake, so the game's own check refuses that copy: the "
    "paper the client puts in the box is not eaten.")
CFG_FLAG(woodchipper_pour_check_off, "woodchipper_pour_check_off", "dev", false, "VOTVCOOP_WOODCHIPPER_POUR_CHECK_OFF",
    "Developer red arm of the woodchipper drill: never set. The host's judge of a client's pour skips its checks of the "
    "gas can in the sender's hand and of the room in the tank, so a pour from an empty hand is taken and an over-pour "
    "fills the chipper past its tank.")
CFG_FLAG(woodchipper_replay_off, "woodchipper_replay_off", "dev", false, "VOTVCOOP_WOODCHIPPER_REPLAY_OFF",
    "Developer red arm of the woodchipper drill: never set. The host does not tell a joiner which chippers are on at "
    "its ready edge, so a rejoined client's copy of a running chipper reads it off.")
CFG_FLAG(gascan_exact_class, "gascan_exact_class", "dev", false, "VOTVCOOP_GASCAN_EXACT_CLASS",
    "Developer red arm of the woodchipper drill's mini arm and the ATV drill's carried_can arm: set on the HOST, never in "
    "play. The woodchipper's and the ATV's judges of a client's pour ask whether the hand holds exactly prop_gascan_C, as "
    "they did before the can's subclasses and the carried can were taken: a pour from a mini can in the hand, or from a big "
    "can carried by the grab, is refused as 'no gas can'.")
CFG_ENUM(woodchipper_drill, "woodchipper_drill", "dev", "off", "off|1|feed|mini", "VOTVCOOP_WOODCHIPPER_DRILL",
    "Drill, the woodchipper lane; set on BOTH peers. 1 -- with the rig's --rejoin for the last arm. The host finds or "
    "spawns a woodchipper beside itself; the client's player is walked to the host by the director, takes a gas can into "
    "its hand and pours it, presses the button, and sends an over-pour and an empty-handed pour. The host prints 'rig: "
    "VERDICT woodchipper PASS|FAIL|INVALID' (arms g1, g2, g5, g6, one line 'woodchipper: ARM <g> PASS|FAIL' each) and "
    "'woodchipper: G1-G6 DONE'; the rejoined client's copy must read the chipper on after its world-ready: 'rig: VERDICT "
    "woodchipper-rejoin PASS|FAIL' and 'woodchipper: DONE on host'. The second life carries "
    "VOTVCOOP_WOODCHIPPER_DRILL_REJOIN=1. feed -- g3 alone after g1 and g2 (a running chipper): a paper dropped over the "
    "hopper must be eaten on the host (its key gone), a clump not in the baseline taken before the drop must appear, and "
    "the client must see the paper gone and that clump present; the host prints 'rig: VERDICT woodchipper-feed "
    "PASS|FAIL|INVALID' (INVALID when the chipper is not running or never eats the paper) and 'woodchipper: FEED DONE on "
    "host'. Red arms: the client's woodchipper_gate_off and woodchipper_intent_off (g2, and g1 for the second), the "
    "host's woodchipper_pour_check_off (g5, g6) and woodchipper_replay_off (the rejoin). It MUTATES the world. Run it on "
    "a fresh host world. mini -- one arm after the chipper is chosen: the client takes a MINI gas can "
    "(prop_gascan_mini_C, a subclass of the plain can) into its hand and pours it into the chipper; the host prints 'rig: "
    "VERDICT woodchipper-mini PASS|FAIL|INVALID', decided at the host's judgement of the pour (its fuel rising, or the "
    "refusal for the can; INVALID only when neither comes in time or the client's copy gained nothing) and 'woodchipper: "
    "MINI DONE on host'. Red: the host's gascan_exact_class, FAIL with the refusal.")
