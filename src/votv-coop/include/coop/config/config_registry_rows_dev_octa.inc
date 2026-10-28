// coop/config/config_registry_rows_dev_octa.inc -- the [dev] rows of the Geom Octa lane: its drill, its red arms and
// the controls of its firing arms.
// Included by config_registry_rows_dev.inc after the explosion rows, before the PI and NB rows, which stay the last
// ones, so each row keeps its place in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(octa_drill, "octa_drill", "dev", "off", "off|births|measure|attack|join", "VOTVCOOP_OCTA_DRILL",
    "Drill, the Geom Octa lane's arms; set on BOTH peers, on a fresh host world. births: the host calls the "
    "gamemode's own spawn verb with the spawn menu's Octa row, the call the night roll shares, and must enrol the "
    "Octa; the client calls the night roll's event, which must be refused, and must materialise the Octa's mirror. "
    "measure: the client's director walks it 2200 cm from the host and the host then spawns an Octa 600 cm from "
    "it, 1300 cm or more from the host; the client stands until three volleys of the host's blasts (one laser makes "
    "several) have reached its explosion lane and walks out; the host prints measure DONE at the first idle after "
    "three verdicts. A measurement of the Octa's liveness with no relay. attack: the same placement, judged on the "
    "relay: the client stands until it has applied three of the host's verdicts, logs the time from each of its own "
    "lasers to the first blast applied after it, and walks out; the host prints attack DONE. join (THREE peers, "
    "run with the rig's --observer): the host walks 2200 cm from client 1, a witness that stands where it spawned, "
    "and spawns the Octa 600 cm from itself timed off client 1's load time, so the Octa aggros the host about 8 s "
    "before the observer (the joiner) is world-ready; the observer's snapshot must carry the Octa and the join Aggro "
    "(a snapshot between attacks ends the arm INCONCLUSIVE), replay it on the host's puppet and apply one verdict, "
    "then says so in the lobby chat; the host walks out on that line and prints join DONE. The firing arms need "
    "damage_probe=1, and every player holds startInvinc for the arm's span.")
CFG_ENUM(octa_red, "octa_red", "dev", "off", "off|no_source|no_parking|no_verdict|no_join", "VOTVCOOP_OCTA_RED",
    "Developer red arm: never set. no_source (HOST): the gamemode's row of the EX spawn catch is skipped, so the "
    "births arm must fail: the host enrols nothing and the client sees no mirror. no_parking (CLIENT): the Octa's "
    "graph watch is left unregistered, so its mirror runs its own wander and the measure arm must show it (the "
    "client logs `[OCTA] mirror ran move`). no_verdict (HOST): the host counts its fire verdicts and sends none, so "
    "the attack arm's client waits for a verdict that never comes (the client logs `[OCTA] verdict timeout`). "
    "no_join (HOST): a joiner's snapshot of an Octa mid-attack sends it no Aggro, so the join arm's joiner gets host "
    "verdicts and no Aggro (it prints `[OCTA-DRILL] FAIL: no aggro at join`).")
CFG_ENUM(octa_measure_target, "octa_measure_target", "dev", "client", "client|host", "VOTVCOOP_OCTA_MEASURE_TARGET",
    "Control for the Octa drill's firing arms, set on BOTH peers: who walks into the Octa's sphere. client (the "
    "default): the client does, as the arm says. host: the host's own director walks the host player 2200 cm from "
    "the client's puppet, the Octa is spawned 600 cm from it, three volleys are read from the host's own verdict "
    "count, and it walks out; the client stands where it spawned; the done line is unchanged. It tells an Octa that "
    "never aggros from one that never sees a puppet.")
CFG_ENUM(octa_spawn_route, "octa_spawn_route", "dev", "sat", "sat|gamemode", "VOTVCOOP_OCTA_SPAWN_ROUTE",
    "Control for the Octa drill's firing arms, set on the HOST: how the Octa is spawned. sat (the default): the dev "
    "NPC spawn, enrolled and sent at its interceptor. gamemode: the gamemode's own spawn verb with the spawn menu's "
    "Octa row, caught at the EX catch and enrolled and sent at the drain a tick later, so the Octa's first aggro, "
    "inside the spawn, finds its spawn unsent and is kept until it is.")
