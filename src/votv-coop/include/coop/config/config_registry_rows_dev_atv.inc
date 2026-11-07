// coop/config/config_registry_rows_dev_atv.inc -- the [dev] rows of the ATV lane: its rig probe, the corrector's
// control arm, the spawn drill and its park, the health drill and its red arm, the eject drill and its red arm, the gate
// probe.
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
CFG_FLAG(atv_gate_probe, "atv_gate_probe", "dev", false, "VOTVCOOP_ATV_GATE_PROBE",
    "Probe (BOTH peers): which of an ATV's own functions fire (gascanFuel, fuelUp, toolboxFix, playerUsedOn, upgradeTake, "
    "Upd Lights, setBrake, unturbo, the lights, brake, turbo and honk input events), one [ATVG] line each -- the role, the "
    "function, the seam that saw it (the script gate or ProcessEvent), the ATV's key, whether the copy is a mirror and the "
    "calling Blueprint function. At most 200 lines per function and seam, the rest counted. Observe only: it never "
    "cancels a call. Off, no watch is registered.")
