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
CFG_FLAG(container_writer_probe, "container_writer_probe", "dev", false, "VOTVCOOP_CONTAINER_WRITER_PROBE",
    "Probe (HOST): every 2 s reads each world container's content hash and says once per container when it "
    "changed with no write the container lane saw (a local write, an applied remote slice or a connect seed's "
    "send), naming a writer nothing watches. On a CLIENT it says once per (container, outermost script function) "
    "when an addObject or takeObj ran on a shared container's mirror outside an asked act. Read-only. Lines "
    "tagged container_writer_probe.")
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
CFG_ENUM(join_prop_loss_drill, "join_prop_loss_drill", "dev", "off", "off|break|breakown|rekey|swing|throwat",
    "VOTVCOOP_JOIN_PROP_LOSS_DRILL",
    "Drill (BOTH peers, run with join_at_host=1): the host puts beer cans on the ground ahead of it before the client "
    "joins; the peers sequence each other by lobby-chat lines and each prints `<role> <arm> PASS|FAIL|INCONCLUSIVE`, "
    "then DONE. break -- once the join is over the client breaks a resting mirror of a host can with an impact no "
    "player caused; the host's can must survive. breakown -- the same with the client's own player as the cause; the "
    "break must reach the host and the can be gone. rekey -- two cans 13.7 cm apart: the client destroys one before "
    "the join's snapshot bracket, then after the bracket the other must still carry its own key, each key must "
    "resolve to its own actor, and the host must still hold both. swing -- a measure: the client takes the axe, walks "
    "within 150 cm of a host can's mirror and swings at it until its copy is gone or 10 swings; the host judges as "
    "in breakown. throwat -- a measure: the client throws a can at a host can's mirror, up to 3 times. Run both "
    "with impact_damage_probe=1 on both peers: the probe lines are the result. Lines tagged [JPL-DRILL].")
CFG_FLAG(prop_damage_gate_off, "prop_damage_gate_off", "dev", false, "VOTVCOOP_PROP_DAMAGE_GATE_OFF",
    "Developer red arm: never set. The client's gate on a prop's impactDamage and fireDamage does nothing, so a client's "
    "own physics can break a mirror of a host prop again and the join prop loss drill's break and fire arms must fail.")
CFG_FLAG(prop_damage_gate_strict, "prop_damage_gate_strict", "dev", false, "VOTVCOOP_PROP_DAMAGE_GATE_STRICT",
    "Developer red arm: never set. The client's gate on a prop's impactDamage and fireDamage ignores that this client "
    "holds, threw or syncs the prop, so a prop it holds no longer breaks on its own count and the join prop loss "
    "drill's held-break arm must fail.")
CFG_ENUM(container_intent_drill, "container_intent_drill", "dev", "off", "off|take|settle|abort|deadline|double|ghost|held|race|two|sale|depart|room|forge|unasked|putrecord|puthand|putfood|puthidden|putnested|putempty|puthover|putreserved|putfull|putfullhand|putdepart|putdeadline|putowed|putjoin|throwhand|throwgrab|throwhostgrab|throwclamp|throwplace|throwjoin|throwdepart|throwtouch",
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
    "prop. forge -- the client writes a record into its mirror twice, a second apart, and publishes each: the host "
    "refuses both as not a birth and sends its truth once (red: container_slice_accept_red=1 on the host). unasked -- "
    "the client calls the game's addObject on its mirror outside any act: the writer probe names it "
    "(container_writer_probe=1 on the client) and the host receives no slice. The put arms pick no container: the host spawns a target (a backpack, a minifridge for putfood) 600 cm "
    "ahead of the client and the item it takes, and each arm MEASURES and prints. putrecord -- the client pockets a "
    "drive and puts it in by the hotbar key. puthand -- it carries a drive past the target and puts it in by hold and "
    "run. putfood -- the same with food and the fridge; the held food's temperature, ripeness, record clock and hash are "
    "printed at the ask and the replay. puthidden -- a hovered slot is hidden and the take key asked: no put may be "
    "asked. putnested -- a gembox that holds a record is put into the target: refused, no put asked. putempty -- an "
    "empty gembox is put in by the hotbar key and the host's stored record is read. puthover -- a slot is hovered "
    "under the parked replay: the put is aborted and the slot's record not taken (red: container_act_no_hover_guard). "
    "putreserved -- the host's own put into the target while the client's grant stands is refused (red: "
    "container_act_no_reservation). putfull / putfullhand -- the target is filled to its volume first and the client's "
    "hotbar-key / hold-and-run put is refused for space. putdepart -- the client leaves with its grant parked "
    "(--rejoin --rejoin-marker \"[CI-DRILL] client putdepart parked\"); the host releases the reservation and inserts "
    "nothing. putdeadline -- the replay is parked past the host's 30 s deadline: the late done is inserted once, a "
    "second done of it is a bad commit, and the next put is answered Lapsed (red: container_act_no_lapse). putowed -- "
    "the host's insert answers NotReady (container_act_insert_notready=1 or 2): the put is given up after 30 tries and the "
    "client's mirror loses the phantom record; with 2 a take is then aborted and its restore must stay owed, never given "
    "up, until the knob is set to 0. putjoin -- two clients (--observer): the first client's put is granted while the "
    "second joins, the second asks the same target and waits at the head, then both commit. throwhand and throwgrab "
    "(run with container_route_census=1 on both peers) MEASURE a client throwing a drive at a shared backpack from about "
    "350 cm, from the hotbar hand or from a physics grab (throwhostgrab: the host grabs and throws, the client only traces): "
    "both peers trace the flight, the first blocking hit, the overlap and the final poses, "
    "and the host prints MEASURED per throw; they fail only on a fixture failure, and on the flight: the thrower's "
    "copy parked in flight, or the host's copy more than the bound from the thrower's at the instants the client "
    "says (red: container_throw_no_elect, container_throw_no_velocity). throwclamp -- throwhand with "
    "container_throw_scale=10: the host's copy must fly no faster than the throw bound (red: container_throw_no_clamp). "
    "throwplace -- the client lets go of a grab and then places a held drive with the game's precision place: neither "
    "carries a throw sequence and the host elects no syncer. throwjoin (--observer) -- a second client joins while the "
    "client's grab throw flies: its copy follows the relayed rows. throwdepart -- the client leaves at the first "
    "admitted row (--rejoin --rejoin-marker \"[CI-DRILL] client throwdepart parked\"): the host takes the prop back at "
    "the last row's velocity and it lands where the host's copy lands. throwtouch -- the host grabs the flying prop "
    "150 ms after the first admitted row: the host overrides the syncer and the thrower's copy follows the host. The "
    "run on throwhand with container_throw_stepped_rows prints whether the host revoked the thrower's trust. "
    "Lines tagged [CI-DRILL]; the "
    "host's DONE line ends it. It MUTATES the world.")
CFG_FLAG(container_act_no_escrow, "container_act_no_escrow", "dev", false, "VOTVCOOP_CONTAINER_ACT_NO_ESCROW",
    "Red arm for the container intent drill (HOST): a grant opens its escrow with the record found but does not "
    "remove it from the container, so a second taker finds it still there and the item is taken twice.")
CFG_FLAG(container_slice_accept_red, "container_slice_accept_red", "dev", false, "VOTVCOOP_CONTAINER_SLICE_ACCEPT_RED",
    "Red arm for the container intent drill's forge arm (HOST): a client's container slice that is not a thrown "
    "container's birth is not refused at the receive edge, so the host applies the forged record and relays it.")
CFG_FLAG(container_act_no_hover_guard, "container_act_no_hover_guard", "dev", false, "VOTVCOOP_CONTAINER_ACT_NO_HOVER_GUARD",
    "Red arm for the put drill's puthover (CLIENT): the replay of a hotbar-key put no longer checks that no slot "
    "is hovered, so a slot hovered after the ask is pressed by the replay and its record is taken unjudged.")
CFG_FLAG(container_act_no_reservation, "container_act_no_reservation", "dev", false, "VOTVCOOP_CONTAINER_ACT_NO_RESERVATION",
    "Red arm for the put drill's putreserved (HOST): a put's grant opens no reservation, so the host's own put "
    "into the same container is not refused while the client's put stands granted.")
CFG_FLAG(container_act_no_lapse, "container_act_no_lapse", "dev", false, "VOTVCOOP_CONTAINER_ACT_NO_LAPSE",
    "Red arm for the put drill's putdeadline (HOST): a sender whose reservation lapsed, or was abandoned late, "
    "is never slowed, so its next put is granted at once instead of answered Lapsed.")
CFG_FLAG(container_act_no_hand_table, "container_act_no_hand_table", "dev", false, "VOTVCOOP_CONTAINER_ACT_NO_HAND_TABLE",
    "Red arm for the put drill's putfood (CLIENT): the per-class table of hold-and-run hash exclusions is empty, so "
    "only the transform and the lifespan are left out and a held food's ticking fields abort the put.")
CFG_INT(container_act_insert_notready, "container_act_insert_notready", "dev", 0, 0, 2, "VOTVCOOP_CONTAINER_ACT_INSERT_NOTREADY",
    "Drill knob for the put drill's putowed (HOST): 1 -- the insert of a put's record answers NotReady every time, so "
    "the put goes the owed path and is given up after 30 tries; an escrow's restore is not affected. 2 -- the host's "
    "restores (Source::Host) answer NotReady too, and the arm's second phase asserts a restore owed that way is never "
    "given up; the drill then sets the knob back to 0 and the record lands.")
CFG_STRING(container_route_census, "container_route_census", "dev", "", "VOTVCOOP_CONTAINER_ROUTE_CENSUS", false,
    "Census (BOTH peers): counts each container verb on a shared world container by the route on its stack (the "
    "slot press, the take key, hold+run, the overlap, the vacuum; none is the game's own logic) and the slices "
    "each route sends, every 30 s and at session end. Lines tagged [CI-CENSUS]. 1 -- the census alone. drill -- "
    "also drives each route once on the client against a container the host places beside it, and reads the "
    "open screen and the record hashes on both peers.")
CFG_FLAG(container_throw_no_elect, "container_throw_no_elect", "dev", false, "VOTVCOOP_CONTAINER_THROW_NO_ELECT",
    "Red arm for the throw arms (HOST): a thrown prop elects no syncer, so the host's own simulation of the throw "
    "is the only flight it streams and the copies part from the thrower's after the first contact.")
CFG_FLAG(container_throw_no_velocity, "container_throw_no_velocity", "dev", false, "VOTVCOOP_CONTAINER_THROW_NO_VELOCITY",
    "Red arm for the throw arms (CLIENT): the drop intent of a thrown prop sends zero velocity, so the host's copy "
    "falls where the throw began while the thrower's flies on.")
CFG_FLAG(container_throw_no_clamp, "container_throw_no_clamp", "dev", false, "VOTVCOOP_CONTAINER_THROW_NO_CLAMP",
    "Red arm for the throwclamp arm (HOST): a client's release and drop-intent velocities are not clamped to the "
    "throw bounds, so the host's copy flies at the forged speed.")
CFG_INT(container_throw_scale, "container_throw_scale", "dev", 1, 1, 100, "VOTVCOOP_CONTAINER_THROW_SCALE",
    "Dev (CLIENT): the velocity a thrown prop's drop intent and release carry is multiplied by this, a forged "
    "throw for the throwclamp arm. 1 = off.")
CFG_FLAG(container_throw_stepped_rows, "container_throw_stepped_rows", "dev", false, "VOTVCOOP_CONTAINER_THROW_STEPPED_ROWS",
    "Red arm for the throw arms (CLIENT and HOST): from its eighth drive row on, the thrower of a thrown prop offsets "
    "each row's position 1000 cm further than the last, so the host's step test fails. The host shows such a row all the same and counts it; a run "
    "of failed rows revokes the thrower's right and the host takes the prop back.")
CFG_FLAG(container_throw_join_nohold, "container_throw_join_nohold", "dev", false, "VOTVCOOP_CONTAINER_THROW_JOIN_NOHOLD",
    "Red arm for the throwjoin arm (HOST): no hold on the second client's world-ready answer, and the throw waits for "
    "its world-ready, so the joiner is seeded before the flight and the arm must fail with 'the joiner was not in flight'.")
