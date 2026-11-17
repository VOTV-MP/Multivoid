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
CFG_FLAG(garbage_state_off, "garbage_state_off", "dev", false, "VOTVCOOP_GARBAGE_STATE_OFF",
    "Developer (a client): the host's state of a garbage container is received and resolved but not applied: the copy's "
    "list, flags and glue stay as they were (the container gaps drill's garbage red).")
CFG_FLAG(nest_record_off, "nest_record_off", "dev", false, "VOTVCOOP_NEST_RECORD_OFF",
    "Developer (a client): a container slice's carried contents of a nested container are not written, so the nested "
    "container is born empty (the container gaps drill's nested red).")
CFG_FLAG(photo_record_off, "photo_record_off", "dev", false, "VOTVCOOP_PHOTO_RECORD_OFF",
    "Developer (any peer): a prop's record is applied with its signals' photos left out (the container gaps drill's photo "
    "red).")
CFG_FLAG(break_request_off, "break_request_off", "dev", false, "VOTVCOOP_BREAK_REQUEST_OFF",
    "Developer (a client): a lethal hit on a container the client authors runs there instead of asking the host (the "
    "container gaps drill's break red).")
CFG_FLAG(hacksaw_request_off, "hacksaw_request_off", "dev", false, "VOTVCOOP_HACKSAW_REQUEST_OFF",
    "Developer (a client): a prop its hacksaw cuts is cut there instead of asked of the host (the hacksaw drill's "
    "red).")
CFG_FLAG(sawn_refuse_all, "sawn_refuse_all", "dev", false, "VOTVCOOP_SAWN_REFUSE_ALL",
    "Developer (the host): every cut a client asks of the host is refused, with the reason that its call did not run, so "
    "the client's saw answers with the game's own hint and ends its cut (the hacksaw drill's refused arm).")
CFG_ENUM(glowstick_drill, "glowstick_drill", "dev", "off", "off|static|world|attached|grab|refused|joiner|joiner_window|hostile",
    "VOTVCOOP_GLOWSTICK_DRILL",
    "Drill, a glowstick stuck to a surface is stuck on every peer; set on BOTH peers, and VOTVCOOP_CAN_DIR on both. The "
    "host spawns the glowstick (and a box for attached) in front of the client's puppet; the client grabs it as the use "
    "key does and holds it tip down over the floor (or the box) until its own ray strikes. static -- the host's copy "
    "must land stuck at the client's point; world -- the same at a floor point that holds no keyed prop: the host's copy must land static "
    "and name no prop; attached -- stuck to the box's side, which the host then lifts: the host's "
    "glowstick follows its box; grab -- the client takes the stuck glowstick again and the host's copy is free when "
    "that hold begins and the client's re-stick after it ends stuck at its point; refused -- with the host's row glowstick_stick_refuse_all the host refuses the stick and its "
    "own record frees the client's copy; joiner -- with the rig's --rejoin the rejoined client (second life: "
    "VOTVCOOP_GLOWSTICK_DRILL_REJOIN=1) must read the stick its save carried stuck at the point; joiner_window -- the same "
    "rejoin, but the host makes the stick after the rejoiner's save is cut and before its world is ready, and the joiner "
    "must land it and make it; hostile -- the client's row "
    "glowstick_far_loc sends a point far from the glowstick and the host must refuse and correct it. The host says "
    "`rig: VERDICT glowstick-<arm> PASS|FAIL|INVALID`. Reds: glowstick_intent_off (static, world), glowstick_attach_key_off "
    "(attached), glowstick_grab_release_off (grab), glowstick_correct_off (refused), glowstick_restore_off (joiner_window). "
    "It MUTATES the world (a glowstick and a box are spawned).")
CFG_FLAG(glowstick_intent_off, "glowstick_intent_off", "dev", false, "VOTVCOOP_GLOWSTICK_INTENT_OFF",
    "Developer (a client): the client's glowstick stick is not sent to the host, so the stick stays on its copy alone "
    "(the glowstick drill's static red).")
CFG_FLAG(glowstick_attach_key_off, "glowstick_attach_key_off", "dev", false, "VOTVCOOP_GLOWSTICK_ATTACH_KEY_OFF",
    "Developer (any peer): a landing that names a struck prop restores the stick as a static one at its point, as a "
    "peer that ignored the key would (the glowstick drill's attached red).")
CFG_FLAG(glowstick_grab_release_off, "glowstick_grab_release_off", "dev", false, "VOTVCOOP_GLOWSTICK_GRAB_RELEASE_OFF",
    "Developer (any peer): a remote hold of a stuck glowstick does not release this peer's copy at its begin (the "
    "glowstick drill's grab red).")
CFG_FLAG(glowstick_correct_off, "glowstick_correct_off", "dev", false, "VOTVCOOP_GLOWSTICK_CORRECT_OFF",
    "Developer (the host): a refused glowstick stick is answered with no record, so the sender's copy stays stuck (the "
    "glowstick drill's refused red).")
CFG_FLAG(glowstick_restore_off, "glowstick_restore_off", "dev", false, "VOTVCOOP_GLOWSTICK_RESTORE_OFF",
    "Developer (any peer): a landing that says stuck makes no stick on this copy (the glowstick drill's joiner red).")
CFG_FLAG(glowstick_stick_refuse_all, "glowstick_stick_refuse_all", "dev", false, "VOTVCOOP_GLOWSTICK_STICK_REFUSE_ALL",
    "Developer (the host): every glowstick stick a client sends is refused (the glowstick drill's refused arm).")
CFG_FLAG(glowstick_far_loc, "glowstick_far_loc", "dev", false, "VOTVCOOP_GLOWSTICK_FAR_LOC",
    "Developer (a client): the point of the stick it sends is a kilometre from where it struck (the glowstick drill's "
    "hostile arm).")
