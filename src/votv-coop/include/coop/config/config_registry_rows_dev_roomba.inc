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
