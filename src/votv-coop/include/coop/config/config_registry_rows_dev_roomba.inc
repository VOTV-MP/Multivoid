// coop/config/config_registry_rows_dev_roomba.inc -- the [dev] rows of the robot cleaner's drill (coop/props/roomba_sync):
// its arm and its reds. Included by config_registry_rows_dev.inc after the sky rows, so each row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(roomba_intent_off, "roomba_intent_off", "dev", false, "VOTVCOOP_ROOMBA_INTENT_OFF",
    "Developer red arm of the robot cleaner drill: never set; the client. A press of a cleaner's switch is refused and "
    "never asked of the host, so the host switches nothing and the drill must FAIL at the fence after the press.")
CFG_FLAG(roomba_drive_claim_off, "roomba_drive_claim_off", "dev", false, "VOTVCOOP_ROOMBA_DRIVE_CLAIM_OFF",
    "Developer red arm of the robot cleaner drill: never set; the host. A running cleaner's drive is not claimed, so its "
    "pose never rides the drive stream and a client's copy does not follow it: the drill must FAIL when the host's "
    "switch-off reaches the client with the copy still near where it stood.")
CFG_ENUM(roomba_drill, "roomba_drill", "dev", "off", "off|1|join", "VOTVCOOP_ROOMBA_DRILL",
    "Drill, the robot cleaner; set on BOTH peers, and VOTVCOOP_CAN_DIR on both. 1 -- the host drops two cleaners; the "
    "client walks to the host with the director and presses the first one's switch: the host must switch it (decided "
    "at the fence, an intent for the second cleaner, answered in order after it), and once the host's cleaner has "
    "driven 200 cm and the host switches it off, the client's copy must have followed 100 cm: 'rig: VERDICT roomba "
    "PASS|FAIL|INVALID' and 'roomba-drill: DONE on host'. join -- with the rig's --rejoin: the host runs a cleaner, "
    "the client leaves, and at the rejoined client's ready edge (second life: VOTVCOOP_ROOMBA_DRILL_REJOIN=1) the host "
    "must have sent a state and the copy must run: 'rig: VERDICT roomba-join PASS|FAIL|INVALID'. Red arms: "
    "roomba_intent_off on the client and roomba_drive_claim_off on the host (1). It MUTATES the world (cleaners are "
    "dropped). Run it on a fresh host world.")
