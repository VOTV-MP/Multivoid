// coop/config/config_registry_rows_dev_prop_intent.inc -- the [dev] rows of the drills for a client's intent on a prop
// that the host judges: the point sack's pay, the hook take-back (drill and reds) and the nametag (drill and reds),
// moved in their order. Included by config_registry_rows_dev.inc where the rows stood, so each of them keeps its place
// in every expansion of the list, the `config: EFFECTIVE` census included. The entry forms are
// config_registry_rows.inc's.

CFG_STRING(sack_drill, "sack_drill", "dev", "", "VOTVCOOP_SACK_DRILL", false,
    "Drill (BOTH peers): a client's point sack is paid by the host, once. run -- the host spawns a sack before the "
    "client's puppet once its world is ready; the client aims and presses it; the host's DONE line ends it.")
CFG_FLAG(point_sack_client_runs, "point_sack_client_runs", "dev", false, "VOTVCOOP_POINT_SACK_CLIENT_RUNS",
    "Developer: a client's point sack runs the game's own body there instead of asking the host (the sack drill's red).")
CFG_STRING(hook_retrieve_drill, "hook_retrieve_drill", "dev", "", "VOTVCOOP_HOOK_RETRIEVE_DRILL", false,
    "Drill (BOTH peers): a client takes back an anchored hook, through the host, once. run -- the host ties a hook "
    "between two cups before the client's puppet once its world is ready and hands it to the hook lane, which adopts "
    "it and sends it to the client; the client aims at its copy until its look takes the hook and presses the use "
    "key. The host's DONE line says its hook went by one granted pickup; the client reports `done ok` in the lobby chat and "
    "the host says the one `rig: VERDICT hook-retrieve` line. join -- the hook is tied before the host player before the client joins, so the "
    "client's copy is the one its save loaded; the client is put beside it and takes it. host / joinhost -- the same "
    "two copies, and the host player walks to its hook and takes it: the host's DONE line, its item in its "
    "inventory with no grant, and the client's copy gone with no item. window -- the host ties the hook before the client joins and takes it between the client's save being cut and its world being ready; the client's join ends with its copy gone.")
CFG_ENUM(hook_retrieve_red, "hook_retrieve_red", "dev", "off", "off|nocollision|clientruns|nojoingone", "VOTVCOOP_HOOK_RETRIEVE_RED",
    "Developer (nojoingone: the host; the others: the client): the hook retrieve drill's reds. nocollision -- an anchored mirror's collision stays off, as "
    "before the lane, so the player's look never takes it (FAIL when the aim fan held every pose); clientruns -- the game's own pickup runs on the client's "
    "copy instead of asking the host (FAIL in the press's tick: nothing was asked, the copy and an item are here); nojoingone -- the host does not name gone, at a joiner's connect, the hooks taken since its save was cut (window mode: the copy stands).")
CFG_STRING(nametag_drill, "nametag_drill", "dev", "", "VOTVCOOP_NAMETAG_DRILL", false,
    "Drill (BOTH peers): a prop's nametag is the host's; the host says the verdict `rig: VERDICT nametag`. run -- the "
    "host spawns a cup before the client's puppet once its world is ready; the client walks to its copy, takes a "
    "nametag into its hand, writes a tag on it and left-clicks. Host half: its cup carries the tag, applied for the "
    "client once; client half: the host's tag for its click came, its copy carries it and the item in its hand is "
    "spent. host -- the host player tags the cup the same way; client half: its copy carries the host's tag. window "
    "-- the host tags a cup before the client joins, between the client's save being cut and its world being ready; "
    "client half: its copy carries the tag at the join's end.")
CFG_FLAG(nametag_client_runs, "nametag_client_runs", "dev", false, "VOTVCOOP_NAMETAG_CLIENT_RUNS",
    "Developer: a client's nametag runs the game's own use there instead of asking the host (the nametag drill's red).")
CFG_FLAG(nametag_no_join_replay, "nametag_no_join_replay", "dev", false, "VOTVCOOP_NAMETAG_NO_JOIN_REPLAY",
    "Developer (the host): the host does not name, at a joiner's connect, the tags put on since its save was cut (the "
    "nametag drill's window red: the joiner's copy stays untagged).")
