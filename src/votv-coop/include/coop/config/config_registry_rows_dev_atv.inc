// coop/config/config_registry_rows_dev_atv.inc -- the [dev] rows of the ATV lane: its rig probe, the corrector's
// control arm, the spawn drill and its park, the health drill and its red arm, the eject drill and its red arm, the gate
// probe and its drill, the riders drill and the part drill.
// Included by config_registry_rows_dev.inc after the fire rows, before the PI and NB rows, which stay the last ones,
// so each row keeps its place in every expansion of the list. The entry forms are config_registry_rows.inc's.
// The drill's arms: sit, grab, occupied, synced, silent, gate, owner, trap; the red arm's values: back, occupied,
// takeover, silence, nonowner, trapbit.

CFG_FLAG(atv_probe, "atv_probe", "dev", false, "VOTVCOOP_ATV_PROBE",
    "Diagnostic probe: the ATV rig baseline (4-body pose, suspension travel, vitals).")
CFG_FLAG(atv_probe_sit, "atv_probe_sit", "dev", false, nullptr,
    "ATV probe DRIVE arm (ONE peer, one shot): seat the local player, release the brake and hold the throttle so the ATV is authored AND moving. Set it on one peer only.")
CFG_FLAG(atv_corrector, "atv_corrector", "dev", true, "VOTVCOOP_ATV_CORRECTOR",
    "ATV pose corrector, ON by default. Set 0 to leave a mirrored ATV entirely alone -- the "
    "CONTROL arm for 'does the mirror settle wrong on its own, or do we settle it wrong'. The "
    "mirror still simulates and still receives state; only the position/velocity correction is "
    "withheld, so the two arms differ in exactly one thing.")
CFG_STRING(atv_spawn_drill, "atv_spawn_drill", "dev", "", "VOTVCOOP_ATV_SPAWN_DRILL", false,
    "Drill (BOTH peers): a client holds one actor per ATV the host holds, none the host does not. run -- one is "
    "spawned once the client's world is ready; join -- while it loads, so its save carries it; park -- as run, with "
    "atv_park_first_spawn=1 on the client; divergence -- as join, destroyed as the stream ends. The DONE line ends it.")
CFG_FLAG(atv_park_first_spawn, "atv_park_first_spawn", "dev", false, "VOTVCOOP_ATV_PARK_FIRST_SPAWN",
    "Developer: the first ATV arrival that would spawn a mirror is parked once per session, as if the world were "
    "not ready, and spawned by the park's hand-back (the ATV spawn drill's park arm).")

CFG_ENUM(atv_health_drill, "atv_health_drill", "dev", "off",
    "off|sit|grab|occupied|synced|silent|gate|owner|trap|spawned|carry_still|synced_peer|join|stream_pace",
    "VOTVCOOP_ATV_HEALTH_DRILL",
    "Drill, the ATV lane's table; set on BOTH peers, the arm names what is done to the ATV the host spawns "
    "beside the players (trap: the world's own ATV). sit: "
    "the client walks to the ATV with the director, takes its seat and leaves it; the host must name the client as "
    "occupant and syncer, then free the seat, then take the ATV back once it rests. grab: the client grabs the ATV "
    "with the grav hand and lets it go; the host must name the client as syncer with the seat free, then take the "
    "ATV back at rest. occupied: the host sits in the ATV; the client's raw sit request must be refused as occupied "
    "and grant nothing. synced: the host grabs the ATV; the client's raw takeover request must be refused as synced "
    "and grant nothing. silent: the client sits and then stops streaming; the host must take the seat and the ATV "
    "back by its silence rule, and the client's own copy must give up the seat. gate: the client sits in the ATV, the "
    "host spawns a native blast above it, and the damage must be cancelled on the host's copy and run on the "
    "client's. owner: the client sits and kills its copy; the host must accept one request and mark the ATV blown by "
    "the client, and the host's own blast must not explode it again. trap: the client sits and the host sets the "
    "story event's trap; the trap must reach the client's copy and fire there as one request, and a second kill after "
    "the host's blast must be accepted. spawned: the host spawns an ATV beside the players and says its key; the "
    "client waits for it in its table (the DONE line prints how long), walks to it and sits. carry_still: the host spawns "
    "an ATV, the client grabs it and holds it still; the host must count 70 still blocks and not hand it back while held, "
    "then hand it back once released. synced_peer (three peers): the client carries the spawned ATV and the observer "
    "(slot 2) grabs it too; the host must refuse the second client's takeover as synced, and the observer must lose the "
    "grab. join (three peers): the client sits in the spawned ATV and the observer joins afterwards; its table must "
    "show the seat and the syncer, and the spawned ATV's own connect-snapshot block must have reached the corrector.stream_pace (three peers): the seated client sends 100 "
    "blocks in one tick; the host's bucket must pace most of them and the observer must see no more than the burst "
    "budget over the normal stream. Each peer prints one DONE line on a "
    "pass, the host's with the times it measured. Run it on a fresh host world; only the client in slot 1 acts.")
CFG_ENUM(atv_health_red, "atv_health_red", "dev", "off",
    "off|back|occupied|takeover|silence|nonowner|trapbit|not_syncer|carry_back|peer_carry",
    "VOTVCOOP_ATV_HEALTH_RED",
    "Developer red arm: never set. back (HOST): the at-rest hand-back of a client-synced ATV is skipped, so the ATV "
    "health drill's `back` phase must fail; range and silence still take an ATV back. occupied (HOST): a sit on an "
    "occupied seat is not refused, so the `occupied` arm must fail. takeover (HOST): a takeover of an ATV the host "
    "carries is not refused, so the `synced` arm must fail. silence (HOST): a silent syncer is not taken back, so the "
    "`silent` arm must fail. nonowner (HOST): the damage gate treats no copy as a non-owner, so the host's copy runs the "
    "blast the `gate` arm needs cancelled and it must fail. trapbit (CLIENT): the stamp's trap bit is ignored, so the "
    "`trap` arm must fail. not_syncer (HOST): a block from a client that does not sync the ATV is not refused, so the "
    "hostile drill's `atv_state_forge` arm must fail. carry_back (HOST): the rest test ignores the grab bit, so a carry "
    "standing still is handed back while held and the `carry_still` arm must fail. peer_carry (HOST): a takeover of an "
    "ATV another client carries is not refused, so the `synced_peer` arm must fail.")
CFG_ENUM(atv_eject_drill, "atv_eject_drill", "dev", "off", "off|host|client",
    "VOTVCOOP_ATV_EJECT_DRILL",
    "Drill, the ATV lane's wheel presence; set on BOTH peers, on a fresh host world. host: the host knocks a rear wheel "
    "off its own idle ATV with the game's own damage path, and the client's copy must lose that wheel once. client: the "
    "client walks to the ATV with the director, sits and knocks the wheel off its copy; the host must keep its wheel and "
    "count the client's claim as skipped. Each peer prints one DONE line on a pass; only the client in slot 1 acts.")
CFG_FLAG(atv_eject_red, "atv_eject_red", "dev", false, "VOTVCOOP_ATV_EJECT_RED",
    "Developer red arm: never set. The receiving copy inverts its wheel presence rule (the host's own presence is "
    "skipped, a client syncer's is consumed), so both arms of the ATV eject drill must fail.")
CFG_ENUM(atv_gate_drill, "atv_gate_drill", "dev", "off",
    "off|green|atv_gate_off|atv_ack_off|atv_judge_off|atv_dup_module|atv_refund_off|atv_publish_off|atv_joiner_rows_off|"
    "atv_bits_off|atv_gate_hold_off|atv_park_off|battery_refund_off|battery_spawn_off|local_destroy_off|battery_in_off|"
    "battery_out_off|battery_publish_off|battery_judge_off|battery_join_off",
    "VOTVCOOP_ATV_GATE_DRILL",
    "Drill (BOTH peers, plus a third client as observer): the client, the host and the observer do each act a player does "
    "at an ATV -- a refuel, a repair, a module fitted and taken off, the seat, lights, brake, turbo and horn, a drive -- and "
    "every step is decided by an event of the ATV host path, never a clock (coop/interactables/atv_drill_events). The client "
    "leaves and rejoins for the joiner and park steps. Ends on rig: VERDICT atv PASS|FAIL|INVALID said by the host, each "
    "step on an `atv-drill: step <n> PASS|FAIL|INVALID` line. green is the whole run; each other value is a red, read only "
    "by the module whose mechanism it disables, and must FAIL at its step: atv_gate_off (the client's rows ask nothing), "
    "atv_ack_off (the host takes a block's vitals unacked), atv_judge_off (every request judged ok), atv_dup_module (a "
    "duplicate fit allowed), atv_refund_off (a refused fit not refunded), atv_publish_off (the host's own acts not "
    "published), atv_joiner_rows_off (a state for an unadopted ATV dropped), atv_bits_off (a block's control bits "
    "ignored, horn included), atv_gate_hold_off (a state applies while the "
    "open-request gate is shut), atv_park_off (a request is judged before its sender's world is ready), "
    "battery_refund_off (a refused battery fit is not refunded), battery_spawn_off (the host spawns no battery on an "
    "accepted unscrew), local_destroy_off (a client keeps the battery prop its own unscrew spawned), battery_in_off and "
    "battery_out_off (the client's battery fit, or unscrew, row asks nothing), battery_publish_off (the host's states carry "
    "no battery), battery_judge_off (a battery fit is not judged against the hand, so a forged one is refunded a battery), battery_join_off (the connect replay and every "
    "answer carry no battery; the live publish keeps it). "
    "The battery steps: b1 a fit, b2 an unscrew (the host spawns the one battery every peer holds), b3 a fit back, b0 a fit "
    "the host refuses as full and refunds, b4 an unscrew it refuses as not held, b6 a forged fit with an empty hand, b5 the "
    "rejoined copy's battery. MUTATES the ATV.")
CFG_ENUM(atv_riders_drill, "atv_riders_drill", "dev", "off",
    "off|on|drive_control|seat_announce_off|body_attach_off|invite_off|rider_stamp_off|mirror_ride_off|mirror_invite_off|"
    "resend_rider_off|birth_seat_off|bind_ride_off|reveal_ride_off|release_seat_off|release_ride_off|getoff_cancel_off|"
    "unseat_latch_off|ragdoll_restore_off|exit_seat_off",
    "VOTVCOOP_ATV_RIDERS_DRILL",
    "Drill (set on the host and both clients; three peers, the observer in slot 1 leaves and rejoins, the driver is slot 2): "
    "the driver sits in an ATV and drives, the host's kerfur rides, and every peer must show the driver's body seated and the "
    "kerfur riding, through a rejoin, the driver's leaving, a ragdoll while seated, the host's own seat and the ATV's "
    "destruction. Every step is decided by an event (coop/interactables/atv_drill_events); the run ends on rig: VERDICT "
    "atv-riders PASS|FAIL|INVALID. on: step 0 (the host spawns a kerfur and sits in the driver's first join window, "
    "and the driver's hidden kerfur mirror must ride at its reveal), then the same kerfur beside the ATV, which the "
    "rejoined observer adopts from the live save (the bind route); drive_control: the run of on without step 0, but the "
    "host's kerfur takes the game's idle verb before the host sits, so the host's own drive is measured with no kerfur on "
    "the ATV (a control, not a red). Each other value is a red, read only by the module whose mechanism it turns off, and "
    "ends FAIL at its step, or INVALID where its route was not reached (ragdoll_restore_off, reveal_ride_off, bind_ride_off): "
    "seat_announce_off (a seat change calls no subscriber, "
    "step 1), body_attach_off (the body is placed, not attached, 1), invite_off (the host's seat-edge invite, 2), "
    "rider_stamp_off (the host's stamp carries no rider, 2), mirror_ride_off (a client's rider subscriber, 2), "
    "mirror_invite_off (a client's game invites a mirror, 2), resend_rider_off (the host's re-send carries no rider, 4), "
    "birth_seat_off (a born body is not seated, 5), bind_ride_off (a bound mirror takes no ride, 5), reveal_ride_off (a "
    "revealed mirror takes no ride, 0; step 0 runs only for on, bind_ride_off and reveal_ride_off), release_seat_off (a seat's end does not release the body, 6), release_ride_off (the "
    "rider's end does not unseat the mirror, 6), getoff_cancel_off (a client's own dismount unseats a mirror, 6), "
    "unseat_latch_off (an unseated mirror keeps its row's latch, so its pose stays frozen, 6), "
    "ragdoll_restore_off (a ragdoll's stop restores no seat, 8), exit_seat_off (an ATV's end announces no change, 9). "
    "MUTATES the ATV.")
CFG_ENUM(atv_part_drill, "atv_part_drill", "dev", "off", "off|wheel",
    "VOTVCOOP_ATV_PART_DRILL",
    "Drill, a client's work on an ATV's wheel; set on BOTH peers and run with join_at_host=1. The host spawns an ATV "
    "beside the players and names it. wheel: five legs, each decided by an event (the host's hand and counter lines, "
    "the peers' said states), never a clock: an unbolt with an empty hand (the host must refuse, the wheel stays), an "
    "unbolt of two wheels with a wrench and the fit of a held wheel (the host must run each, both peers must show the "
    "same tyre mask and wheel props), and a second fit forged on the same hold (the host must refuse it and give "
    "nothing back). Either peer's FAIL or INVALID is told to the other; each peer ends on one `rig: VERDICT atv-part "
    "PASS|FAIL|INVALID` line, the host's PASS after its own counters (ran 3, refused 2, refunded 0). A time bound "
    "says INVALID only. Reds: atv_part_red=cancel_off (CLIENT) and atv_part_red=tool_judge_off (HOST), and "
    "hand_spend_drill=hand_spend_no_latch (HOST), each of which must FAIL at its own leg.")
CFG_ENUM(atv_part_red, "atv_part_red", "dev", "off", "off|cancel_off|tool_judge_off",
    "VOTVCOOP_ATV_PART_RED",
    "Developer red arm: never set. cancel_off (CLIENT): the four ATV_C watches ask nothing and cancel nothing, so a "
    "wrench's last step runs on the client's own copy and the part drill's first leg must FAIL (no ask was made). "
    "tool_judge_off (HOST): an unbolt is not judged against the tool the sender's hand holds, so the drill's "
    "empty-hand unbolt must FAIL (the host ran it).")
