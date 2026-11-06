// coop/config/config_registry_rows_dev_cremator.inc -- the [dev] rows of the cremator probe and the lever lane.
// Included by config_registry_rows_dev.inc after its last row, as _dev_vulkan.inc is, so the rows keep their place in
// every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(cremator_probe, "cremator_probe", "dev", "off", "off|1|host|placeonly", "VOTVCOOP_CREMATOR_PROBE",
    "Probe (a cremator's own burn on a coop player). 1 -- the CLIENT walks into the nearest cremator's firebox, "
    "shuts the door and pulls the lever on its own copy. host -- the HOST opens the nearest cremator's door, "
    "sends the client's puppet to the firebox with the admin verb's teleport order, and once the cremator's own "
    "begin-overlap sees the puppet shuts the door and pulls the lever on its own copy. placeonly -- the CLIENT "
    "moves nothing: it waits for the host's move and reports its health (pair it with host on the host). One "
    "line a second; lines tagged 'cremator-probe:'; "
    "the DONE line ends it ('HOST DONE' on the host). It MUTATES the world.")
CFG_FLAG(cremator_state_off, "cremator_state_off", "dev", false, "VOTVCOOP_CREMATOR_STATE_OFF",
    "Developer red arm of the cremator lever drill: never set. The host reports no cremator state, so its lever run "
    "sends no edge and no connect snapshot and the client's copy stays idle.")
CFG_FLAG(cremator_intent_anim_off, "cremator_intent_anim_off", "dev", false, "VOTVCOOP_CREMATOR_INTENT_ANIM_OFF",
    "Developer red arm of the cremator lever drill: never set. The host's judge of a client's handle press skips its "
    "check that its own handle is at rest, so two presses close together both pull the lever.")
CFG_FLAG(cremator_intent_reach_off, "cremator_intent_reach_off", "dev", false, "VOTVCOOP_CREMATOR_INTENT_REACH_OFF",
    "Developer red arm of the cremator lever drill: never set. The host's judge of a client's handle press skips its "
    "reach check, so a press from beyond the handle's reach pulls the lever.")
CFG_ENUM(cremator_drill, "cremator_drill", "dev", "off",
    "off|1|g3|g3low|g5|cremate|cremate-running|cremate-low|cremate-running-low|cremate-survive|cremate-transit|cremate-dropped|cremate-exempt|"
    "cremate-self|cremate-host|g2|g2-double|g2-far", "VOTVCOOP_CREMATOR_DRILL",
    "Drill (BOTH peers). 1 -- the cremator's lever run crosses. The host pulls the lever on its copy and the client's "
    "copy must read it on, its door latched, and then off again (G1); with the rig's --rejoin the host pulls it on once "
    "more and the client, leaving and coming back, must read it on after its world-ready (G4). The host prints "
    "'cremator-drill: VERDICT PASS|FAIL <arm>' from what the client signalled; G1 ends at 'cremator-drill: G1 DONE', "
    "the rejoin run at 'cremator-drill: DONE on host'. The second life carries VOTVCOOP_CREMATOR_DRILL_REJOIN=1. Red "
    "arm: the host's cremator_state_off. g3 -- a client in the host's running cremator dies by the machine: the host "
    "sends the client to the firebox with the admin verb's teleport order, shuts the door and pulls the lever, and "
    "the client signals its death; the host prints 'cremator-drill: VERDICT PASS|FAIL g3' and 'cremator-drill: G3 "
    "DONE'. Red arm: the host's cremator_hit_off. g3low -- the g3 burn with the HOST's own health set to 5 before its "
    "lever: the burn loop's achievement call then runs on the host with the client's puppet as its actor, and the host "
    "must refuse it; the host prints 'cremator-drill: VERDICT PASS|FAIL g3low' (the client died and the refusals "
    "went up by at least one) and 'cremator-drill: G3LOW DONE'. Red arm: the host's player_damage_no_refusal. g5 -- "
    "the same burn with the client's own copy watched: the host "
    "prints 'cremator-drill: VERDICT PASS|FAIL g5' (the client died, its copy's actors read 0 at every look, and the "
    "guard refused at least one collection; the fireball births are reported, not judged) and 'cremator-drill: G5 "
    "DONE'. Red arm: the client's cremator_client_guard_off. cremate -- the host dispatches '/cremate #<client>' "
    "through the command dispatcher as the operator: the burn volume must take the client in, the client dies by "
    "the machine, the host's reply says it was cremated and the client's end reason is MV-H35 (red arm: the host's "
    "cremate_landing_off). cremate-running -- the same with the host's cremator already running. cremate-low -- the "
    "same with the client's own health set to 5 first: one burn tick ignites and kills it, and it must still end "
    "cremated and kicked. cremate-running-low -- both: the host's cremator runs first and the client is at 5. "
    "cremate-survive -- "
    "the host pulls the lever back once the run is on: the client survives, no kick. cremate-transit -- the client's "
    "cremate_transit_die: it dies on the order's receipt and never moves, so the verb ends 'died before landing', no kick. "
    "cremate-dropped -- the client's cremate_drop_order: the order never applies, so 'did not land', no kick. "
    "cremate-exempt, cremate-self and cremate-host (the first with the rig's --observer): client 1 holds "
    "multivoid.cremate, granted by the host's own /mv and revoked at the end, and sends '/cremate' for the "
    "observer, who holds multivoid.cremate.exempt, for itself, or for the host; each is refused with its own line "
    "and nobody moves. g2, g2-double and g2-far -- the CLIENT walks to the handle with the director, aims until its own "
    "look-at reads the handle and presses as a player's E: the host judges the intent and runs its lever, counted at "
    "the body. g2 presses on, then off; g2-double presses twice 0.2 s apart and the host's lever must run once (red "
    "arm: the host's cremator_intent_anim_off); g2-far presses from 1500 cm or more away with the look-at forced "
    "and the host must refuse it, then a near press must run (red arm: the host's cremator_intent_reach_off). Each "
    "prints 'cremator-drill: VERDICT PASS|FAIL <arm>' and '<ARM> DONE' on the host. It MUTATES the world.")
CFG_FLAG(cremate_landing_off, "cremate_landing_off", "dev", false, "VOTVCOOP_CREMATE_LANDING_OFF",
    "Developer red arm of the /cremate drill: never set. The verb does not watch the cremator's burn volume, so a "
    "player it sent is never seen to land: the trip ends 'did not land' and nobody is kicked.")
CFG_FLAG(cremate_transit_die, "cremate_transit_die", "dev", false, "VOTVCOOP_CREMATE_TRANSIT_DIE",
    "Developer knob of the /cremate drill: never set. This client's health is set to 0 on the receipt of the "
    "host's teleport order and the move is not applied, so the host reads its death before it lands.")
CFG_FLAG(cremate_drop_order, "cremate_drop_order", "dev", false, "VOTVCOOP_CREMATE_DROP_ORDER",
    "Developer knob of the /cremate drill: never set. This client does not apply the host's teleport order, so it "
    "never arrives and the verb gives the trip up after its bound of poses.")
CFG_FLAG(cremator_client_guard_off, "cremator_client_guard_off", "dev", false, "VOTVCOOP_CREMATOR_CLIENT_GUARD_OFF",
    "Developer red arm of the cremator guard drill: never set. A client's cremator copy collects the sitter, opens its "
    "door and turns its valve on its own account, so a client standing in the host's running cremator burns in its "
    "own copy too.")
CFG_FLAG(cremator_hit_off, "cremator_hit_off", "dev", false, "VOTVCOOP_CREMATOR_HIT_OFF",
    "Developer red arm of the cremator burn drill: never set. The host sends no hit for a cremator's damage or ignite "
    "on a client's puppet (the puppet still takes none), so a client standing in the host's running cremator keeps "
    "its health.")
