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
    "Developer red arm of the body-push drill: never set. The host sees a body's push and streams nothing for it, so in the "
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
CFG_FLAG(fireext_claim_off, "fireext_claim_off", "dev", false, "VOTVCOOP_FIREEXT_CLAIM_OFF",
    "Developer red arm of the fire extinguisher drill's runaway arm (host): never set. The host does not claim an "
    "extinguisher while its runaway thrust flies it, so the flight (started once the let-go's own drive has ended) is not "
    "streamed and the client's copy stays where it fell: the drill must say FAIL by the copies' divergence, far past the "
    "bound. Run it with body_push_off set too, so no body's push streams the flight.")
CFG_FLAG(fireext_cancel_off, "fireext_cancel_off", "dev", false, "VOTVCOOP_FIREEXT_CANCEL_OFF",
    "Developer red arm of the fire extinguisher drill's runaway arm (client): never set. A client's copy is not refused its "
    "own shoot, so the leg where its own copy takes damage lets it thrust and the drill must say FAIL at that leg. A host "
    "leg that rests apart first says INVALID, since the cancel was not what it turned.")
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
CFG_ENUM(fireext_drill, "fireext_drill", "dev", "off", "off|carry|short|client|join|runaway",
    "VOTVCOOP_FIREEXT_DRILL",
    "Drill (BOTH peers): a wall-mounted fire extinguisher taken off through the use key's own grab "
    "chain. carry: the host carries it a few metres once a client's join is over, and the client runs "
    "the laptop exit's setPropProps on its carried copy once, which the drive must re-latch. short: "
    "the host lets go the moment it holds it, then re-sends the closed hold's pose for a moment, which "
    "every receiver must drop. client: the client carries it and the host watches. join: the host "
    "takes it off while a joiner is still loading. runaway: the host lets go of it and damages it through "
    "its own addDamage, which starts the runaway thrust (turned upside down first, so the thrust flies it); "
    "then the client's own copy takes the damage and must not thrust; the client says its copy's rest pose and whether "
    "its copy ever thrust, and the host compares within 1 cm and 1 degree: rig: VERDICT fireext-runaway "
    "PASS, FAIL or INVALID. Both peers print every extinguisher they have and every move of one.")
