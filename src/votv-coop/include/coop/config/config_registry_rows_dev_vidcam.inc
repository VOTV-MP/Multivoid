// coop/config/config_registry_rows_dev_vidcam.inc -- the [dev] rows of the video camera hint drill and its red.
// Included by config_registry_rows_dev.inc after the disc rows. The entry forms are config_registry_rows.inc's.

CFG_ENUM(vidcam_hint_drill, "vidcam_hint_drill", "dev", "off", "off|host|client", "VOTVCOOP_VIDCAM_HINT_DRILL",
    "Drill (the named peer): once it is in a gameplay world with its player, it puts a video camera into its hand "
    "through the game's own hold step and waits for the game's own overlay to show the viewfinder, then checks that "
    "the state the HUD's hint reads (coop/player/vidcam_hint) says so too; it empties the hand and checks the same "
    "when the overlay is gone. Says `rig: VERDICT vidcam-hint PASS` when both agreed, `FAIL` at the first "
    "disagreement, `INVALID` when the game never changed its overlay within a bound. Lines tagged "
    "[VIDCAM-HINT-DRILL].")
CFG_FLAG(vidcam_hint_red, "vidcam_hint_red", "dev", false, "VOTVCOOP_VIDCAM_HINT_RED",
    "Drill red (with vidcam_hint_drill): the watch on the camera manager's updUI is not registered, so the HUD's "
    "overlay state is never published and the drill must say FAIL at the viewfinder. Off = the watch stands, as "
    "designed.")
