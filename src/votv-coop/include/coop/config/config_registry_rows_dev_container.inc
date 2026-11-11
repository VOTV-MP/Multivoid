// coop/config/config_registry_rows_dev_container.inc -- the [dev] container rows that sat together in the dev row list,
// moved in their order: the join drill with its two red arms, the opener probe and the
// view drill, the writer probe and the two red arms of the drone sack drill. Included by config_registry_rows_dev.inc where they stood, so each row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(container_join_drill, "container_join_drill", "dev", false, "VOTVCOOP_CONTAINER_JOIN_DRILL",
    "Drill (BOTH peers): the host extracts an item while a joiner is connected but not world-ready; the joiner "
    "then reports a content hash per world container and the host judges it. Lines tagged [CONTAINER-JOIN] and "
    "[CONTAINER-PARITY]; the host's DONE line ends it. It MUTATES the world.")
CFG_FLAG(container_retry_red, "container_retry_red", "dev", false, "VOTVCOOP_CONTAINER_RETRY_RED",
    "Red arm for the container join drill: a container send refused with no world-ready peer is re-armed "
    "every sweep, as before the fix, and counted.")
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
CFG_FLAG(offhand_drill, "offhand_drill", "dev", false, "VOTVCOOP_OFFHAND_DRILL",
    "Drill (BOTH peers): a client takes a crowbar into its main hand, runs the off-hand key's handler so the off hand holds it, "
    "then drops the off hand's item; the host must resolve the dropped prop's key to a live prop, and its hand_item lane logs "
    "the off-hand mirror. Lines tagged [OFFHAND-DRILL].")
CFG_FLAG(container_take_drill, "container_take_drill", "dev", false, "VOTVCOOP_CONTAINER_TAKE_DRILL",
    "Drill (BOTH peers): a client walks to a world container with at least two records, opens it on the inventory "
    "screen and moves the first record into its bag as the screen's drag does (the bag's addItemAdvanced, then the "
    "container's removeInventoryItem); the container must hold one record fewer and the bag one more, and still do "
    "so a few seconds later. The host logs every change of a container's record count. Lines tagged [CTAKE-DRILL]; "
    "the client's DONE line carries the verdict.")
CFG_FLAG(drone_cargo_take_drill, "drone_cargo_take_drill", "dev", false, "VOTVCOOP_DRONE_CARGO_TAKE_DRILL",
    "Drill (BOTH peers): the host fills the delivery drone's cargo with two records copied off a world container; a client "
    "walks to the drone, moves the first record into its bag as the inventory screen's drag does (the bag's addItemAdvanced, "
    "then the cargo's removeInventoryItem); the cargo must hold one record fewer and the bag one more, and still do so a few "
    "seconds later, and the host must read the cargo one record fewer. Lines tagged [DRONE-CARGO-DRILL]; the client's DONE "
    "line and the host's PASS or FAIL line carry the verdicts.")
CFG_FLAG(atv_trunk_drill, "atv_trunk_drill", "dev", false, "VOTVCOOP_ATV_TRUNK_DRILL",
    "Drill (BOTH peers, run with join_at_host=1): the host fills the first ATV's trunk with two records copied off a world "
    "container and sets that ATV down beside its player; a client takes the first record into its bag as the inventory "
    "screen's drag does (the bag's addItemAdvanced, then the trunk's removeInventoryItem), then opens the trunk on the "
    "screen and drops that bag item on its canvas (the screen's enddrag); each leg must stand on the client and on the "
    "host, at a second read too. Last the host adds a record itself and the client's mirror must show it. It MUTATES the "
    "ATV's trunk. Lines tagged [ATV-TRUNK-DRILL]; the client's DONE line and the host's PASS or FAIL line carry the verdicts.")
CFG_FLAG(container_stack_drill, "container_stack_drill", "dev", false, "VOTVCOOP_CONTAINER_STACK_DRILL",
    "Drill (BOTH peers): the host clears a world container and seeds it with a stack of 6 and two stacks of 3 of one item; a "
    "client walks there, opens it on the inventory screen and drags one unit off the stack of 6 with the right button (the "
    "screen's own beginDrag on that stack's widget, which runs takeFromStack), then merges the two stacks of 3 as the canvas's "
    "combine action does (addToStack on one, then the other leaves). Each leg must stand on the client and on the host, at a "
    "second read too. It MUTATES the container. Lines tagged [CSTACK-DRILL]; the client's DONE line and the host's PASS or "
    "FAIL line carry the verdicts.")
CFG_ENUM(join_prop_loss_drill, "join_prop_loss_drill", "dev", "off", "off|break|swing|heldbreak|fire|rekey|twin|stack|late|throwat",
    "VOTVCOOP_JOIN_PROP_LOSS_DRILL",
    "Drill (BOTH peers, run with join_at_host=1): the host puts beer cans on the ground ahead of it before the client "
    "joins; the peers sequence each other by lobby-chat lines and each prints `<role> <arm> PASS|FAIL|INCONCLUSIVE`, "
    "then DONE. break -- once the join is over the client calls a resting mirror's impactDamage with no hitting actor; "
    "its copy and the host's can must stand. fire -- the same with fireDamage. swing -- the client takes the axe, walks "
    "within 150 cm of a host can's mirror and swings at it until its copy is gone or 20 swings, each aimed at the copy's place a moment before, the first after the host has a pose for it; "
    "both copies must go, the client's by its own break as the elected syncer and the host's by that destroy. "
    "heldbreak -- the client takes a host can into its hand with the director's grab and calls its impactDamage; the "
    "copy must go and the host's can with it. rekey -- two cans 13.7 cm apart: the client destroys one before the "
    "join's snapshot bracket, then after the bracket the other must still carry its own key, each key must resolve to "
    "its own actor, and the host must still hold both. twin -- the client destroys its copy of a can and spawns one "
    "of its own at its place before the bracket; after it key A must resolve to that can. stack -- three cans 13.7 cm "
    "apart on X: the client destroys the middle copy before the bracket; after it the other two must keep their own "
    "keys and the middle key must resolve to a third actor. late -- the host places a can on the client's line that "
    "its bracket closed; it must show with no wait. throwat -- a measure: the client throws a can at a host can's "
    "mirror, up to 3 times. Run all with impact_damage_probe=1 on both peers. Lines tagged [JPL-DRILL].")
CFG_FLAG(prop_damage_gate_off, "prop_damage_gate_off", "dev", false, "VOTVCOOP_PROP_DAMAGE_GATE_OFF",
    "Developer red arm: never set. The client's gate on a prop's impactDamage and fireDamage does nothing, so a client's "
    "own physics can break a mirror of a host prop again and the join prop loss drill's break and fire arms must fail.")
CFG_FLAG(prop_damage_gate_strict, "prop_damage_gate_strict", "dev", false, "VOTVCOOP_PROP_DAMAGE_GATE_STRICT",
    "Developer red arm: never set. The client's gate on a prop's impactDamage and fireDamage ignores that this client "
    "holds, threw or syncs the prop, so a prop it holds no longer breaks on its own count and the join prop loss "
    "drill's held-break arm must fail.")
CFG_FLAG(hit_intent_refuse_prop, "hit_intent_refuse_prop", "dev", false, "VOTVCOOP_HIT_INTENT_REFUSE_PROP",
    "Developer red arm: never set (HOST). The host takes no prop as the target of a client's hit: it elects no swinger "
    "and drives its own copy of the prop instead. The client's break is never granted, so the join prop loss drill's "
    "swing arm must fail: the client's copy follows the host and both copies of the can stand.")
CFG_FLAG(bracket_position_wait_off, "bracket_position_wait_off", "dev", false, "VOTVCOOP_BRACKET_POSITION_WAIT_OFF",
    "Developer red arm: never set (CLIENT). Inside the snapshot bracket a host spawn that matches a neighbour by "
    "position binds it on arrival, as it did before the bracket's end was awaited, so the bracket's eid order decides "
    "whose actor each key gets and the join prop loss drill's rekey arm must fail.")
CFG_ENUM(container_intent_drill, "container_intent_drill", "dev", "off", "off|throwhand|throwgrab|throwhostgrab|throwclamp|throwplace|throwjoin|throwdepart|throwtouch",
    "VOTVCOOP_CONTAINER_INTENT_DRILL",
    "Drill (BOTH peers): the throw arms. A client throws a drive at a shared backpack from about 350 cm. throwhand and "
    "throwgrab (run with container_route_census=1 on both peers) MEASURE a client throwing from the hotbar hand or "
    "from a physics grab (throwhostgrab: the host grabs and throws, the client only traces): "
    "both peers trace the flight, the first blocking hit, the overlap and the final poses, "
    "and the host prints MEASURED per throw; they fail only on a fixture failure, and on the flight: the thrower's "
    "copy parked in flight, or the host's copy more than the bound from the thrower's at the instants the client "
    "says (red: container_throw_no_elect, container_throw_no_velocity). throwclamp -- throwhand with "
    "container_throw_scale=10: the host's copy must fly no faster than the throw bound (red: container_throw_no_clamp). "
    "throwplace -- the client lets go of a grab and then places the held item with the game's place (dropItem_mainhand on "
    "the surface): neither carries a throw sequence and the host elects no syncer. throwjoin (--observer) -- a second "
    "client joins while the client's grab throw flies: its copy follows the relayed rows. throwdepart -- the client "
    "leaves at the first admitted row (--rejoin --rejoin-marker \"[CI-DRILL] client throwdepart parked\"): the host "
    "takes the prop back at the last row's velocity and it lands where the host's copy lands. throwtouch -- the host "
    "grabs the flying prop 150 ms after the first admitted row: the host overrides the syncer and the thrower's copy "
    "follows the host. The run on throwhand with container_throw_stepped_rows prints whether the host revoked the "
    "thrower's trust. Lines tagged [CI-DRILL]; the host's DONE line ends it. It MUTATES the world.")
CFG_FLAG(container_slice_accept_red, "container_slice_accept_red", "dev", false, "VOTVCOOP_CONTAINER_SLICE_ACCEPT_RED",
    "Red arm (HOST): a client's container slice that is not a thrown "
    "container's birth is not refused at the receive edge, so the host applies the forged record and relays it.")
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
