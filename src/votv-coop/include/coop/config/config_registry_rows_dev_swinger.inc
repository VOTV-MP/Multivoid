// coop/config/config_registry_rows_dev_swinger.inc -- the [dev] row of the swinger drill: what a dragged hinge door
// shows on the other peers. Included by config_registry_rows_dev.inc at the place the row stood, as _dev_nb.inc is,
// so the row keeps its place in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(swinger_drill, "swinger_drill", "dev", "off", "off|measure|observe|rejoin", "VOTVCOOP_SWINGER_DRILL",
    "Drill (host and clients): what a dragged hinge door shows on the other peers. measure: the host and "
    "the acting client. The client walks to a non-lockable lid its NavMesh reaches, one whose game Key differs "
    "between processes first, grabs it through the player's own grab chain and feeds the player's "
    "input_mouseXY dispatcher, the delta doubling to a cap on each of four signs in turn until its own door "
    "has turned 15 degrees (a lid's spring holds it at its open limit); no sign moving it ends the run "
    "INVALID. It lets go, and once the door has come to rest says in the lobby chat where it rests. observe: "
    "a second client, which only watches. A watching peer prints PASS when its copy of the door turned at "
    "least 10 degrees during the drag and, at rest, lies within 3 degrees and 5 cm of the client's, else "
    "FAIL, and says whether the lid's game Key matched. Every peer prints a baseline line per lid, then "
    "every 250 ms a sample line for each lid that moved. Lines tagged [SWING-DRILL]; the client's DONE "
    "line ends its part, after the host's verdict. rejoin: measure, whose client ends its first life on \"client LIFE DONE\"; "
    "the host says in the chat, as each client becomes world-ready, go (no verdict yet) or rest (its verdict's door, "
    "where it rests); the client waits for that word before it picks a lid, and on rest drags nothing and judges its copy of "
    "the lid against the host's (PASS within 3 degrees and 5 cm); \"rejoin DONE\" ends it.")
CFG_ENUM(swinger_drill_red, "swinger_drill_red", "dev", "off", "off|no_parent_claim", "VOTVCOOP_SWINGER_DRILL_RED",
    "Developer red arm: never set. no_parent_claim (the acting client): a dragged swinger's parent prop is not "
    "claimed for the prop syncer, so the measure arm's host must FAIL on the parent synced test (0 rows).")
