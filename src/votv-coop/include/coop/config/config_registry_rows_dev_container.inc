// coop/config/config_registry_rows_dev_container.inc -- the [dev] container rows that sat together in the dev row list,
// moved in their order: the contents selftest, the join drill with its two red arms, the opener probe and the
// view drill, the writer probe and the two red arms of the drone sack drill. Included by config_registry_rows_dev.inc where they stood, so each row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(container_selftest, "container_selftest", "dev", false, "VOTVCOOP_CONTAINER_SELFTEST",
    "Autonomous selftest: container contents sync. It MUTATES the world -- it extracts an item on"
    " each peer -- so it is armed per run from the environment, like the hand-drop one.")
CFG_FLAG(container_join_drill, "container_join_drill", "dev", false, "VOTVCOOP_CONTAINER_JOIN_DRILL",
    "Drill (BOTH peers): the host extracts an item while a joiner is connected but not world-ready; the joiner "
    "then reports a content hash per world container and the host judges it. Lines tagged [CONTAINER-JOIN] and "
    "[CONTAINER-PARITY]; the host's DONE line ends it. It MUTATES the world.")
CFG_FLAG(container_retry_red, "container_retry_red", "dev", false, "VOTVCOOP_CONTAINER_RETRY_RED",
    "Red arm for the container join drill: a container send refused with no world-ready peer is re-armed "
    "every sweep, as before the fix, and counted.")
CFG_FLAG(drone_sale_watch_off, "drone_sale_watch_off", "dev", false, "VOTVCOOP_DRONE_SALE_WATCH_OFF",
    "Red arm for the drone sack drill (HOST): the watch that publishes the drone's emptied container after a sale "
    "is not registered, so the clients' copies keep the sold items until something else writes the container.")
CFG_FLAG(container_judge_red, "container_judge_red", "dev", false, "VOTVCOOP_CONTAINER_JUDGE_RED",
    "Red arm for the drone sack drill (HOST): a client container slice whose base is stale but not zero is "
    "accepted (a zero base, and a live read that fails, are still refused), as when the host judged against what "
    "it last published. With drone_sale_watch_off, the sold items come back into the drone's container.")
CFG_FLAG(container_writer_probe, "container_writer_probe", "dev", false, "VOTVCOOP_CONTAINER_WRITER_PROBE",
    "Probe (HOST): every 2 s reads each world container's content hash and says once per container when it "
    "changed with no write the container lane saw (a local write, an applied remote slice or a connect seed's "
    "send), naming a writer nothing watches. Read-only. Lines tagged container_writer_probe.")
CFG_FLAG(container_parity_red, "container_parity_red", "dev", false, "VOTVCOOP_CONTAINER_PARITY_RED",
    "Red arm for the container parity report (the CLIENT): flips one bit of the first row's content hash "
    "before sending, so the host's judge must report a mismatch.")
CFG_FLAG(container_opener_probe, "container_opener_probe", "dev", false, "VOTVCOOP_CONTAINER_OPENER_PROBE",
    "Probe (host): once each client's world is up and it has a body, every drone, dronesack and ATV with "
    "the container it opens, the lookups both ways that must name the pair, and each client's reach to "
    "both, again when a client stands at an opener; a verdict per pass. Read-only.")
CFG_FLAG(container_view_drill, "container_view_drill", "dev", false, "VOTVCOOP_CONTAINER_VIEW_DRILL",
    "Drill (client): walks to the ATV, opens its container through the game's own openPropInv, then walks "
    "back; the view must stay open while the ATV is within reach and close on the tick it leaves it. Run it "
    "with join_at_host=1. Lines tagged [CVIEW-DRILL]; the client's DONE line, carrying the verdict, ends it.")
CFG_ENUM(container_intent_drill, "container_intent_drill", "dev", "off", "off|take|settle|abort|deadline|double|ghost|held|race|two|sale|depart|room|putrecord|puthand|putfood|puthidden|putnested|putempty",
    "VOTVCOOP_CONTAINER_INTENT_DRILL",
    "Drill (BOTH peers): both pick the same shared container (the smallest element id, within 5000 cm of the player, "
    "that holds two records or more and that a nav route reaches; race, two, sale and room take one whose first record "
    "is unique first; with none reached, the nearest); "
    "the client walks to it, opens it and presses its first slot, and prints the reply-to-commit time and the "
    "container and own counts; the host prints the container's count and the act holds on change. take -- the "
    "honest take. settle -- the press comes 5 s after the screen opened. abort -- the replay is parked and the "
    "screen closed under it, so the take is aborted. deadline -- the replay is parked past the host's hold "
    "deadline, so the client aborts the stale grant. double -- abort, sent twice. held -- the replay is parked "
    "20 s and still runs. ghost -- a record the host never had is pressed. race -- the client's grant, then the host's own "
    "press of the same container; the client keeps the item and the host does not. two -- two clients press the same "
    "record (run with --observer): one grant and one Gone. sale -- the host's reply is held and a raw clear of the "
    "container (a stand-in for a sale) comes between the grant and the replay; the client keeps the item. depart -- "
    "the replay is parked and the rig drops the client (--rejoin --rejoin-marker \"[CI-DRILL] client depart "
    "parked\"); the host's record returns. room -- the client's own inventory has no room, so the item stays a world "
    "prop. The put arms pick no container: the host spawns a target (a backpack, a minifridge for putfood) 600 cm "
    "ahead of the client and the item it takes, and each arm MEASURES and prints. putrecord -- the client pockets a "
    "drive and puts it in by the hotbar key. puthand -- it carries a drive past the target and puts it in by hold and "
    "run. putfood -- the same with food and the fridge; the held food's temperature, ripeness and record hash are "
    "printed at the ask and the replay. puthidden -- a hovered slot is hidden and the take key asked: no put may be "
    "asked. putnested -- a backpack that holds a record is put into the target: refused, no put asked. putempty -- an "
    "empty backpack is put in by the hotbar key and the host's stored record is read. Lines tagged [CI-DRILL]; the "
    "host's DONE line ends it. It MUTATES the world.")
CFG_FLAG(container_act_no_escrow, "container_act_no_escrow", "dev", false, "VOTVCOOP_CONTAINER_ACT_NO_ESCROW",
    "Red arm for the container intent drill (HOST): a grant opens its escrow with the record found but does not "
    "remove it from the container, so a second taker finds it still there and the item is taken twice.")
CFG_STRING(container_route_census, "container_route_census", "dev", "", "VOTVCOOP_CONTAINER_ROUTE_CENSUS", false,
    "Census (BOTH peers): counts each container verb on a shared world container by the route on its stack (the "
    "slot press, the take key, hold+run, the overlap, the vacuum; none is the game's own logic) and the slices "
    "each route sends, every 30 s and at session end. Lines tagged [CI-CENSUS]. 1 -- the census alone. drill -- "
    "also drives each route once on the client against a container the host places beside it, and reads the "
    "open screen and the record hashes on both peers. drill-before -- the take the host refuses.")
