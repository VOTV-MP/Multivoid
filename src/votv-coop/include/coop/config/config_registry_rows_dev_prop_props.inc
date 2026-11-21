// coop/config/config_registry_rows_dev_prop_props.inc -- the [dev] rows of the prop-properties tool's lane (its red
// arms) and of the prop-rest lane's drills and their red arms. Included by config_registry_rows_dev.inc after the woodchipper rows, so each row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(prop_props_gate_off, "prop_props_gate_off", "dev", false, "VOTVCOOP_PROP_PROPS_GATE_OFF",
    "Developer red arm of the prop-properties drill: never set. A client's press of the toolgun's prop-properties tool "
    "is not cancelled and not sent to the host, so it runs on the client's copy alone and the host's copy keeps its "
    "flags.")
CFG_FLAG(prop_props_judge_off, "prop_props_judge_off", "dev", false, "VOTVCOOP_PROP_PROPS_JUDGE_OFF",
    "Developer red arm of the prop-properties drill: never set. The host's judge of a client's press skips its check of "
    "the player's right to the tool, so a press from a player the host has withheld it from is applied.")
CFG_FLAG(prop_props_park_selftest, "prop_props_park_selftest", "dev", false, "VOTVCOOP_PROP_PROPS_PARK_SELFTEST",
    "Selftest (either peer, once per process, in the first tick): the order in which the prop-properties tool's states "
    "wait for a held copy and replay, on stated inputs -- a newer state that reached the copy directly is never "
    "overwritten by the older one parked for it, the newest parked state wins, a gone copy is forgotten, a copy held past "
    "the warning keeps its state for the hold's end. Prints \"prop_tool_park selftest: ALL PASS\" or FAIL.")
CFG_FLAG(prop_props_park_selftest_break, "prop_props_park_selftest_break", "dev", false, "VOTVCOOP_PROP_PROPS_PARK_SELFTEST_BREAK",
    "Red arm of the park selftest: never set. The first case does not supersede the older parked state, so the "
    "replay applies it over the newer press and the selftest must print FAIL.")
CFG_FLAG(body_push_drill, "body_push_drill", "dev", false, "VOTVCOOP_BODY_PUSH_DRILL",
    "Drill (BOTH peers, default off): a client's body walks into a prop and both copies must come to rest alike. The host "
    "spawns a woodchipper before the client's puppet on measured open ground, taller than the step the client's character "
    "climbs; the client's director walks it into the woodchipper five times. At each push's rest the client says its copy's "
    "pose and the host compares it with its own, within 1 cm and 1 degree. The host says rig: VERDICT body-push PASS, FAIL "
    "(a push rested apart: said at that push) or INVALID.")
CFG_FLAG(body_push_off, "body_push_off", "dev", false, "VOTVCOOP_BODY_PUSH_OFF",
    "Developer red arm of the body-push drill: never set outside a drill's run (the constraint-join drill requires it, so a "
    "body's push never gives a member its row). The host sees a body's push and streams nothing for it, so in the "
    "drill's last leg, where the host's own player walks into the prop, the host's copy and the client's rest each by their "
    "own physics (or no host row opens) and the drill must say FAIL at that leg. A client's push is its own claim and is not "
    "affected; one that rests apart first says INVALID, since it is not what this red turns.")
CFG_ENUM(coast_drill, "coast_drill", "dev", "off", "off|letgo|unelected|join", "VOTVCOOP_COAST_DRILL",
    "Drill (BOTH peers): a crate a hand lets go of must come to rest alike on both peers. The host spawns a crate on "
    "measured open ground; letgo: the host grabs it by the director's aimed grab and lets go (a plain release); unelected: "
    "the client does, and the host, which elects no syncer for a release with no throw, flies it; join: the host holds it "
    "until a joiner is loading, lets go, and the crate rests before the joiner's world is up (run with the rig's rejoin: "
    "the client's life 1 prints REJOIN-NOW once the host holds the crate). The client says its copy's rest pose and the "
    "host compares it with its own, within 1 cm and 1 degree, and requires the drive's End to have reached the client's "
    "copy. rig: VERDICT coast-letgo, coast-unelected or coast-join PASS, FAIL or INVALID.")
CFG_FLAG(coast_letgo_off, "coast_letgo_off", "dev", false, "VOTVCOOP_COAST_LETGO_OFF",
    "Developer red arm of the coast drill: never set. The host flies no let-go: a prop a hand lets go of falls by each "
    "peer's own physics and no End reaches the client's copy, so the drill must say FAIL at the copies' gap. Run it with "
    "body_push_off set too, or the host's own body flies the crate in the let-go's place (the drill then says FAIL by "
    "the cause). The coast drill's join arm has no red: its snapshot carries the host's pose whatever flew the crate.")
CFG_INT(letgo_release_delay_ms, "letgo_release_delay_ms", "dev", 0, 0, 400, "VOTVCOOP_LETGO_RELEASE_DELAY_MS",
    "Developer knob of the coast drill's letgo arm (a client; 0 = off): the client holds each reliable prop release it "
    "receives for this many milliseconds before applying it, so the host's let-go row reaches the copy while the hand "
    "still holds it, the order a hand's release and a drive row sometimes take on their own. Kept under 500, the hand "
    "lane's stream-silence release, which would end the hand's drive before the held release arrives. The arm says "
    "INVALID when no yield was kept pending on the client.")
CFG_FLAG(letgo_unyield_off, "letgo_unyield_off", "dev", false, "VOTVCOOP_LETGO_UNYIELD_OFF",
    "Developer red arm of the coast drill's letgo arm: never set. The client drops the yield it kept pending on a hand's "
    "release instead of lifting it, so the let-go row's End is dropped as it was and the copy stays where the hand left "
    "it: run with letgo_release_delay_ms set, the drill must say FAIL at the copies' gap.")
CFG_FLAG(fireext_claim_off, "fireext_claim_off", "dev", false, "VOTVCOOP_FIREEXT_CLAIM_OFF",
    "Developer red arm of the fire extinguisher drill's runaway arm (host): never set. The host does not claim an "
    "extinguisher while its runaway thrust flies it, so the flight (started once the let-go's own drive has ended) is not "
    "streamed and the client's copy stays where it fell: the drill must say FAIL by the copies' divergence, far past the "
    "bound. Run it with body_push_off set too, so no body's push streams the flight.")
CFG_FLAG(fireext_cancel_off, "fireext_cancel_off", "dev", false, "VOTVCOOP_FIREEXT_CANCEL_OFF",
    "Developer red arm of the fire extinguisher drill's runaway arm (client): never set. A client's copy is not refused its "
    "own shoot, so the leg where its own copy takes damage lets it thrust and the drill must say FAIL at that leg. A host "
    "leg that rests apart first says INVALID, since the cancel was not what it turned.")
CFG_FLAG(fireext_names_off, "fireext_names_off", "dev", false, "VOTVCOOP_FIREEXT_NAMES_OFF",
    "Developer red arm of the fire extinguisher drill (both peers): never set. The thrust lane does not resolve the "
    "extinguisher's class names, and the drill never does, so every read of an extinguisher is false: once the lane has "
    "ticked, the drill logs FAIL on both peers and stops, and the runaway arm's host says the FAIL verdict.")
CFG_FLAG(fireext_ask_off, "fireext_ask_off", "dev", false, "VOTVCOOP_FIREEXT_ASK_OFF",
    "Developer red arm of the fire extinguisher drill's runaway arm (client): never set. A client's copy is still refused "
    "its own shoot but asks the host for nothing, so the leg where the client's copy takes damage leaves the host's copy "
    "still and the drill must say FAIL at that leg, with no answer from the host.")
CFG_FLAG(fireext_thrust_say_off, "fireext_thrust_say_off", "dev", false, "VOTVCOOP_FIREEXT_THRUST_SAY_OFF",
    "Developer red arm of the fire extinguisher drill's runaway arm (host): never set. The thrust lane sends no thrust "
    "state (op 0): a client's copy shows nothing of the host's runaway, and the drill must say FAIL in the first leg at "
    "the host's thrust end, with no thrust read on the client.")
CFG_FLAG(fireext_mirror_ray_off, "fireext_mirror_ray_off", "dev", false, "VOTVCOOP_FIREEXT_MIRROR_RAY_OFF",
    "Developer red arm of the fire extinguisher drill's spray arm (either peer): never set. No peer casts the game's "
    "extinguishing ray from a hand mirror, so a fire burning in the world of a peer that did not spray burns on and the "
    "drill must say FAIL at the end of the spray.")
CFG_FLAG(fireext_snapshot_off, "fireext_snapshot_off", "dev", false, "VOTVCOOP_FIREEXT_SNAPSHOT_OFF",
    "Developer red arm of the fire extinguisher drill's spray_join arm (host): never set. The join snapshot sends a joiner "
    "neither the sprays nor the thrusts, so the joiner shows none of them and the drill must say FAIL at the snapshot's end.")
CFG_FLAG(collision_probe, "collision_probe", "dev", false, "VOTVCOOP_COLLISION_PROBE",
    "Probe (either peer, default off): logs every primitive component of the local pawn and of each puppet with its collision "
    "flag, object type and responses to PhysicsBody, Pawn, WorldDynamic, WorldStatic and Vehicle, when first seen and two "
    "seconds later; the body-push drill adds the woodchipper's root and, per push, each pawn component against it (the pair's "
    "mutual response, the engine's overlap flag, the distance). Lines start [COLLISION-PROBE]; no verdict reads them.")
CFG_FLAG(end_trace, "end_trace", "dev", false, "VOTVCOOP_END_TRACE",
    "Measure (either peer, default off): on the receiver of a prop's drive End, for the two ticks after the End put the copy "
    "to sleep, logs whether the body reads awake, its linear and angular velocity and its penetration (the lowest point of "
    "its bounds against the surface a short down-trace finds under it), and about three seconds on where it rests against "
    "the End pose's height. Lines start [END-TRACE]; no verdict reads them.")
CFG_FLAG(open_row_trace, "open_row_trace", "dev", false, "VOTVCOOP_OPEN_ROW_TRACE",
    "Measure (either peer, default off): for every drive row this peer authors and holds open (a client's claim, a host coast), "
    "once a second logs the author's body awake flag (1 awake, 0 asleep, -1 unread) and how far its pose moved since the last "
    "line, to show whether a prop leaning on a pawn stays awake. Lines start [ROW-TRACE]; no verdict reads them.")
CFG_FLAG(body_push_claim_off, "body_push_claim_off", "dev", false, "VOTVCOOP_BODY_PUSH_CLAIM_OFF",
    "Developer red arm of the body-push drill (a client): never set. This client's body push sends no claim, so the push's "
    "drive row is the host's own coast, not the pusher's peer's, and the drill must say FAIL on that row owner at the first "
    "push that touches the prop. The copies' rest is not what fails: the host's coast alone brings them together.")
CFG_ENUM(fireext_drill, "fireext_drill", "dev", "off", "off|carry|short|client|join|runaway|spray|spray_join",
    "VOTVCOOP_FIREEXT_DRILL",
    "Drill (BOTH peers): a wall-mounted fire extinguisher taken off through the use key's own grab "
    "chain. carry: the host carries it a few metres once a client's join is over, and the client runs "
    "the laptop exit's setPropProps on its carried copy once, which the drive must re-latch. short: "
    "the host lets go the moment it holds it, then re-sends the closed hold's pose for a moment, which "
    "every receiver must drop. client: the client carries it and the host watches. join: the host "
    "takes it off while a joiner is still loading. runaway: the host lets go of it and damages it through "
    "its own addDamage, which starts the runaway thrust (turned upside down first, so the thrust flies it); "
    "the client's copy shows the host's thrust (it reads active and the host's charge) and never runs its own; then a "
    "fresh extinguisher stands over the client, the client's copy takes the damage, asks the host, and the host's copy "
    "runs away and the two rest within 1 cm and 1 degree: rig: VERDICT fireext-runaway PASS, FAIL or INVALID. spray: the "
    "client sprays the extinguisher in its hand at a paper it lit and the host's copy of that paper goes out too, then "
    "the host sprays at another and the client's copy goes out; the client's hand and the host's count of its charge "
    "agree: rig: VERDICT fireext-spray PASS, FAIL or INVALID. spray_join: the host sprays and sets an extinguisher "
    "running away before the client is launched (the rig's client-after), and the joiner must show both once its join is "
    "done and show the spray's end: rig: VERDICT fireext-spray-join PASS, FAIL or INVALID. Both peers print every "
    "extinguisher they have and every move of one.")
CFG_ENUM(miner_drill, "miner_drill", "dev", "off", "off|use|claim|record|join", "VOTVCOOP_MINER_DRILL",
    "Drill (BOTH peers): a GPU mining rig, each arm ending on rig: VERDICT miner-<arm> PASS, FAIL or INVALID, said by the host. "
    "use: a client uses a GPU on a rig and the host grants it; a forged use with an empty hand is refused and the client's "
    "copy reads the host's count again; a use on a full rig is refused and its GPU is given back beside the client; "
    "a flood of asks, each with a hold behind it, is granted the host's burst of 4. "
    "claim: a client's claim of a rig's points is asked of the host, whose balance is paid. record: a client's forged record "
    "of a rig it did not drop is refused, and the record of a rig it throws lands. join: a rig built before the client joins "
    "shows the host's counts and power on the joiner (the rig's client-after is the host's \"join rig built\" line).")
CFG_FLAG(cleanup_trace, "cleanup_trace", "dev", false, "VOTVCOOP_CLEANUP_TRACE",
    "Developer trace (both peers, default off): each props clean-up call the session runs for a peer that left and for "
    "the session's end is logged as \"cleanup-trace: <phase> <the call>\" before it runs, so two builds that move those "
    "calls between files can be compared by their two lists of lines, which must be equal. It changes no behaviour.")
CFG_FLAG(miner_hold_judge_off, "miner_hold_judge_off", "dev", false, "VOTVCOOP_MINER_HOLD_JUDGE_OFF",
    "Developer red arm of the mining rig drill (host): never set. The host does not ask the hand lane whether the sender holds "
    "the item (no hold read, no use spent), so a use sent with an empty hand is granted and the drill must say FAIL at the "
    "forged use.")
CFG_FLAG(miner_bucket_off, "miner_bucket_off", "dev", false, "VOTVCOOP_MINER_BUCKET_OFF",
    "Developer red arm of the mining rig drill (host): never set. A client's uses and claims have no budget, so a flood of asks "
    "is all granted and the drill must say FAIL at the flood's count.")
CFG_FLAG(miner_refusal_state_off, "miner_refusal_state_off", "dev", false, "VOTVCOOP_MINER_REFUSAL_STATE_OFF",
    "Developer red arm of the mining rig drill (host): never set. A refused ask is answered but the host's state of the rig is "
    "not sent to the asker, so the asker's copy keeps what its own use or edit did and the drill must say FAIL at that copy.")
CFG_FLAG(miner_claim_ask_off, "miner_claim_ask_off", "dev", false, "VOTVCOOP_MINER_CLAIM_ASK_OFF",
    "Developer red arm of the mining rig drill (client): never set. A client's claim is cancelled and not asked of the host "
    "(it says \"claim: not asked\"), so the host's balance is not paid and the drill must say FAIL there.")
CFG_FLAG(miner_record_gate_off, "miner_record_gate_off", "dev", false, "VOTVCOOP_MINER_RECORD_GATE_OFF",
    "Developer red arm of the mining rig drill (host): never set. The class gate on a client's rig record is not registered, "
    "so a client's forged record lands on the host's rig and the drill must say FAIL at the host's count.")
CFG_FLAG(miner_refund_off, "miner_refund_off", "dev", false, "VOTVCOOP_MINER_REFUND_OFF",
    "Developer red arm of the mining rig drill (host): never set. A use the host refuses is not given back (it says \"is "
    "not refunded (miner_refund_off)\"), so the client's GPU is lost and the drill must say FAIL when the client finds no "
    "new GPU beside it.")
CFG_FLAG(miner_snapshot_off, "miner_snapshot_off", "dev", false, "VOTVCOOP_MINER_SNAPSHOT_OFF",
    "Developer red arm of the mining rig drill (host): never set. The host sends a joiner no rig's state at its ready edge "
    "(it says \"snapshot withheld\"), so the joiner's copy keeps what its record carried and the drill must say FAIL at it.")
CFG_ENUM(dingus_drill, "dingus_drill", "dev", "off", "off|find|pocket|loaded|join", "VOTVCOOP_DINGUS_DRILL",
    "Drill (BOTH peers): a dingus (Maxwell) found on one peer is found on every peer, each arm ending on rig: VERDICT dingus "
    "PASS, FAIL or INVALID, said by the host. find: the client grabs a dingus and the host's copy is found, then the host grabs "
    "one and the client's copy is found. pocket: the client grabs a dingus, lets go, holds it and drops it, and the host's copy "
    "born from the drop is found; the host then holds that copy and drops it, and the client's mirror of the host's drop is "
    "found at its bind, its set holding the key only from the host's word; the host does the same with a dingus of its own and "
    "the client's mirror of its drop is found. loaded: a dingus the host's world loads found, held and dropped by the client with no grab, is "
    "found on the host's copy born from the drop. join: a dingus the host found before the client joined, held and dropped by "
    "the host after the join, is found on the joiner's mirror at its bind (the rig's client-after is the host's \"join dingus "
    "found\" line).")
CFG_ENUM(dingus_red, "dingus_red", "dev", "off", "off|report|broadcast|birth_mark|bind_mark|birth_seed|snapshot", "VOTVCOOP_DINGUS_RED",
    "Developer red arm of the dingus drill (both peers): never set. Each value withholds one effect of the lane, so the arm that "
    "judges it must say FAIL. report: a client sends no report of its find. broadcast: the host sends no DingusFound for a find "
    "made now. birth_mark: the host does not mark a copy born under a found key. bind_mark: a client does not mark a copy it binds "
    "under a found key. birth_seed: the host's birth drain ignores a copy born found. snapshot: the host sends a joiner no key.")
CFG_ENUM(constraint_join_drill, "constraint_join_drill", "dev", "off", "off|c1|c2|c3", "VOTVCOOP_CONSTRAINT_JOIN_DRILL",
    "Drill (BOTH peers, default off; run with mp.py smoke on a fresh host): every prop a physics constraint ties to a driven "
    "prop gets a drive row of its own. The host builds a nailed set on measured open ground and waits until every drive on it "
    "is closed. c1: a chain of three boxes nailed A-B and B-C, C 250 cm from A (past a body's push); the host carries A 300 cm "
    "away from C and lets go. c2: a pair nailed A-B; the host walks more than 5 m away, the client carries A 300 cm and lets go "
    "(the host judges B's row at the client's let-go), then the client leaves and rejoins and its copies must stand where the "
    "host's rest. c3: a chain of six boxes with constraint_join_drill_cap=2; the host carries A, and the members past the cap "
    "are owed a row. Run with body_push_off set on every arm (the host says INVALID without it). The host says FAIL at the "
    "leader's End when a member never had a row, or its row was opened by anything but the join; the client says PASS when every "
    "member's copy stood at its End pose. rig: VERDICT constraint-join PASS, FAIL or INVALID.")
CFG_ENUM(constraint_join_off, "constraint_join_off", "dev", "off", "off|join|hand|owed", "VOTVCOOP_CONSTRAINT_JOIN_OFF",
    "Developer red arm of the constraint-join drill (host): never set. join: a prop that gets a row owes the props tied to it "
    "nothing, so c1 must say FAIL at the leader's End. hand: the host's own take of a prop and a client's take owe nothing "
    "(a row's own birth still does), so c2 must say FAIL at the client's let-go. owed: a member past the join cap is dropped, "
    "never owed, so c3 must say FAIL at the leader's End.")
CFG_INT(constraint_join_drill_cap, "constraint_join_drill_cap", "dev", 0, 0, 32, "VOTVCOOP_CONSTRAINT_JOIN_DRILL_CAP",
    "Drill knob of the constraint-join drill (host): never set outside it. Above 0, a joined row opens only while fewer props "
    "than this coast (the join's own cap, in place of the drive's maximum less the let-go reserve), so a short chain reaches "
    "the cap. 0 leaves the cap as built.")
CFG_ENUM(kdress_drill, "kdress_drill", "dev", "off", "off|puton|takeoff|pat|born|join", "VOTVCOOP_KDRESS_DRILL",
    "Drill (BOTH peers): an Omega's outfit and its pat. The host says rig: VERDICT kdress PASS, FAIL or INVALID. The host "
    "spawns the targets near the client's puppet; a FAIL is said where it is decided and nothing after it runs; a bound says "
    "INVALID. puton: the client holds the glasses and puts them on an Omega through the player's own use; the host's copy "
    "wears them once its commit is taken. takeoff: the host dresses an Omega, the client takes the glasses off through "
    "upgradeTake; both copies lack them and one glasses item stands beside the Omega on both peers. pat: the client pats an "
    "Omega and the host's copy plays the pat montage; the host pats and the client's copy plays it. born: the host spawns an "
    "Omega and dresses it in the same tick; the client's copy wears the row at its bind, told by the updateDrip send. join: "
    "the host dresses an Omega, "
    "takes the row off after the joiner's capture and before its world-ready; the joiner's copy lacks it after the host's "
    "join send (run it with the rig's client-after set to the host's [KDRESS] [H] J dressed line).")
CFG_ENUM(kdress_red, "kdress_red", "dev", "off", "off|apply|ask|pat|snapshot", "VOTVCOOP_KDRESS_RED",
    "Developer red arm of the Omega outfit drill (both peers): never set. Each value withholds one effect of the lane, so "
    "the arm that judges it must say FAIL. apply: the host writes no row at a put-on's commit (arm puton). ask: a client's "
    "take-off cancels and asks nothing (arm takeoff). pat: the host sends no pat to a client (arm pat). snapshot: "
    "the host sends a joiner no outfit (arm join).")
