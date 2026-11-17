// coop/config/config_registry_rows_dev_crowbarpry.inc -- the [dev] rows of the crowbar pry drill: the drill and its red arm.
// Included by config_registry_rows_dev.inc where they stood, after the pry drill's row, so each row keeps its place
// in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(crowbarpry_drill, "crowbarpry_drill", "dev", "off", "off|open|hand",
    "VOTVCOOP_CROWBARPRY_DRILL",
    "Drill (BOTH peers, the client also takes prop_birth_key_probe=1): a client pries a wall-attachable sign the host spawned beside "
    "it with the game's own activatePrying and ends the pry. open: the pry opens, the board comes free and the crowbar "
    "the pry hands back must exist on the host (the host says PASS; the client says FAIL when that crowbar's birth is "
    "not sent). hand: the pry is given up, the crowbar goes back into the hand and no world crowbar may reach the host "
    "(the client says PASS, the host FAIL if one appears). Run with --rejoin or --rehost: the second life pries again.")
CFG_FLAG(crowbarpry_authorship_off, "crowbarpry_authorship_off", "dev", false, "VOTVCOOP_CROWBARPRY_AUTHORSHIP_OFF",
    "Developer red arm: never set (CLIENT). 1 stops the pry's End from being admitted as a player's birth, so the "
    "crowbar drill's open arm must FAIL on the client with \"the pry's crowbar was not sent\".")
