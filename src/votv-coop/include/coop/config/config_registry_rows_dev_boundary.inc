// coop/config/config_registry_rows_dev_boundary.inc -- the receive boundary's drills: the drills that
// forge what a peer sends and prove the receiving peer's boundary refuses it, grouped as one concept.
// Included from config_registry_rows_dev.inc the way _dev_port.inc is, so each row keeps its place in
// every expansion of the list. Entry forms are config_registry_rows.inc's.
//
// One rule for every row here: a row arms the SENDING side only. The receiving side's gates have no
// knob, no dev branch and no env check, so a drill exercises the real refusal; a drill's red, the
// proof that it can fail, is run on bytes without the gate (the commit before the fix, or a lane that
// deletes the gate in source and never lands), never by a switch in the shipped DLL.

// The admission drill, client side only (the rule above): a knob on the host would make the drill's
// own pass conditional on a bypass existing in shipped code.
CFG_ENUM(auth_drill, "auth_drill", "dev", "off", "off|corrupt|silent|mismatch",
    "VOTVCOOP_AUTH_DRILL",
    "Sabotage THIS CLIENT's identity proof, to prove the host refuses it. corrupt = "
    "send a flipped signature; silent = open the exchange and never answer. Both must "
    "leave this peer unseated and with no save. mismatch = pretend we were sent to a "
    "DIFFERENT host than the one that answered, which THIS peer must refuse itself "
    "-- it is the only arm whose refusal is ours, not the host's. "
    "Drill only.")

// The hostile-peer drill: a client sends deliberately malformed coop packets of a chosen kind and the
// receiving host judges whether the boundary refused them WITHOUT stalling the session, the verdict
// printed by the mod so the rig reads a marker. Set on BOTH peers: the client sends, and on the host
// the row only names the judge; it changes nothing the host's gates do. Off by default.
CFG_ENUM(hostile_drill, "hostile_drill", "dev", "off",
    "off|signal_park|chat_ill_formed|prop_convert|reliable_flood|repeat_join|repeat_world_ready|"
    "repeat_save_request|record_bad_classes|bad_alarm|bad_prop_key|bad_prop_spawn|bad_hand_class|"
    "bad_hand_family|keep_latest|chat_flood|world_latest|kerfur_flood|remover_far|remover_hidden|remover_flood|"
    "act_far|act_gone|act_wrongfamily|act_bad_commit|act_notyet|act_flood|act_flood_early|"
    "explosion_malformed|explosion_duplicate|explosion_unknown_shot|explosion_consumed|explosion_no_launcher|"
    "explosion_origin|explosion_reach|explosion_not_seated|explosion_no_explosive|explosion_hand_bound|"
    "explosion_story|explosion_wait_behind_shot|explosion_not_ready|explosion_evicted|explosion_flood|"
    "explosion_rejoin|explosion_self_clamp|"
    "ignite_malformed|ignite_no_tool|ignite_torch_attempt|ignite_torch_ignite|ignite_reach|ignite_not_rocket|"
    "ignite_not_ready|ignite_flood|"
    "atv_state_forge|atv_seat_forge|atv_takeover_forge|atv_explode_forge|"
    "cord_malformed|cord_bad_op|cord_reach|cord_flood|cord_dead_sender|"
    "coffee_malformed|coffee_wrong_class|coffee_reach|coffee_flood|coffee_forged_record|coffee_dead_sender",
    "VOTVCOOP_HOSTILE_DRILL",
    "Receive-boundary drill (BOTH peers). signal_park: a client floods refused saved-signal rows "
    "(size 0, so the game's own gate rejects each) and the host must keep them from filling the apply "
    "park and pausing the receive loop, while an honest saved signal sent behind them still applies. "
    "chat_ill_formed: a client sends an ill-formed UTF-8 chat line; the host must refuse it and keep a "
    "well-formed one sent behind it at the drills' chat spacing. prop_convert: a client sends a trash re-skin, which only the host "
    "authors, then an honest trash spawn behind it on the same lane; the host must not apply the re-skin, "
    "and judges once the spawn is bound. reliable_flood: a client sends a burst under the most an honest "
    "client may send at once, then one far over it, each followed by a chat line; the host must keep the link "
    "after the first and end it after the second, and the client judges by the code it was given. Pin the "
    "client's net.sendrate_kbs so the bursts arrive at the link's pace. repeat_join: a joined client re-sends "
    "its own Join twice, then a chat line; the host must answer no repeat Join and keep the link, and judges "
    "by its own answered-Join count at the chat line. repeat_world_ready: a joined client re-sends its "
    "world-ready announce six times; the host must run only the burst's worth of connect replays and then end "
    "this client, and the client judges by the code it was given. repeat_save_request: a joined client asks "
    "the host for its world save a second time; the host must end this client, and the client judges by the "
    "code it was given. record_bad_classes: a joined client sends one PropSaveDataIntent for a real covered "
    "prop's key whose record carries ~1024 unknown class names; without the fix the host walks the whole object "
    "array per name resolving each, and the host judges by its own whole-array walk count across a sentinel "
    "spawn (a large rise FAILs, zero PASSes). bad_alarm: a joined client sends a burst of AlarmState; without "
    "the fix an unbound alarm trigger re-walks the array per message. A world with the trigger bound cannot "
    "stage it, so this arm FAILs on an observed walk and is otherwise INCONCLUSIVE (the alarm fix is proven by "
    "code). bad_prop_key: a joined client sends 64 PropDestroys with no element id and keys no prop holds, "
    "then a sentinel spawn; bad_prop_spawn: 16 keyed spawns of a loaded class away from every instance of "
    "it, then the sentinel. For both the host judges by its own whole-array walk count at the sentinel (at or "
    "past half the walks a scan per message would pay FAILs). bad_hand_class: a joined client says its hand "
    "holds the engine's base actor class, which is not a prop, then sends a chat line; the host must not keep "
    "that class for the slot nor build a mirror of it, and judges at the chat line. bad_hand_family: the same, "
    "naming the per-player inventory container's class in capitals (class names compare without case); the "
    "host must not keep it. keep_latest: a joined client sends, in one tick, 120 skin, nameplate, nick colour and "
    "hand changes each alternating two values, then a last value of each; the host judges by its own counts that "
    "it applied no more of each than the kind's budget pays over the drain it measured, then that each last value "
    "still owed was paid (a kind "
    "the burst left holding its last value owes nothing and is not judged). chat_flood: a joined client sends ten "
    "chat lines in one tick, then one line once the first is back and the debt is waited out, then four lines 0.9 s "
    "apart once that one is back and the bucket refilled; the host must commit the burst's first line only, the "
    "next line, and three of the four, and judges by its own lobby record. world_latest: a joined client sends, in "
    "one tick, 120 alarm, bed and laptop-power values alternating two, then a last one; the host must apply each at "
    "its budget, still apply the last, and its laptop settle on it. kerfur_flood: a joined client sends, in one "
    "tick, 121 kerfur menu commands and 121 form conversion requests, each naming an unused host-range id; the host "
    "must run each kind at its pace and no more than its cap, judged by its own per-sender counts. remover_far: a "
    "joined client asks the host to remove a real prop from a point 2000 uu off its camera; the host must answer "
    "OutOfSight and nothing else. remover_hidden: a joined client asks for a host prop its camera cannot see, from its "
    "real camera; the host must answer OutOfSight. remover_flood: a joined client sends, in one tick, 121 remover "
    "requests each naming an unused host-range id; the host must serve them at the lane's pace and no more than its "
    "cap, judged by its own per-sender counts. act_*: A3-D2a's take-or-use requests forged against the host. "
    "explosion_*: a joined client forges shots and explosion requests against the host, one refusal reason per arm "
    "(or the shot table, the queue, the eviction, the not-ready and the rejoin mechanisms); the host judges by "
    "accounting, every message one accept or one refusal, against its counters since its session start. "
    "ignite_*: a joined client forges salt rocket light requests against the host, one refusal reason per arm "
    "(or the not-ready, the flood and the accepted torch light); the host judges by the same accounting. "
    "atv_*: a joined client forges raw ATV messages against the host, in phases: atv_state_forge (blocks for an ATV the "
    "host syncs), atv_seat_forge (ill-formed, unknown, out-of-reach and paced sits and a leave), atv_takeover_forge "
    "(takeovers of a seated ATV and a burst past the pace) and atv_explode_forge (blast requests from a non-syncer, "
    "ill-formed, and twice from the syncer); the host judges each phase by its counters, and a counter the phase does "
    "not name must not move. "
    "cord_*: a joined client forges cord link claims against the host: cord_malformed (a claim one byte short, a "
    "cord key past the wire), cord_bad_op (a canonical and a seed end from a client), cord_reach (a claim on a cord "
    "2000 uu away), cord_flood (20 claims in one tick, 12 taken and 8 refused by the rate window) and "
    "cord_dead_sender (a well-formed claim sent while the client's own player is dead: refused as dead and answered); "
    "the host judges by its counters, every message one outcome, every answered claim also counted by the client. "
    "coffee_*: a joined client forges coffee presses and a record against the host: coffee_malformed (a press one "
    "byte short, a key past the wire), coffee_wrong_class (a press on a crowbar's key), coffee_reach (a press on a "
    "maker 2000 uu away), coffee_flood (20 presses in one tick: 8 run, 12 refused for a full queue), "
    "coffee_forged_record (the client's record of the host's maker with hasCup set: refused, nothing published) and "
    "coffee_dead_sender (a well-formed press sent while the client's own player is dead: refused as dead); the "
    "host judges by the coffee lane's counters, every message one outcome. "
    "Drill only.")

// The act drill: an honest client takes and uses shared props through the real verbs, and the host judges what it
// answered and committed. Set on BOTH peers (the host spawns and judges, the client acts); it arms the sending side
// only, like the rows above.
CFG_ENUM(prop_act_drill, "prop_act_drill", "dev", "off", "off|honest|race|depart|edge", "VOTVCOOP_PROP_ACT_DRILL",
    "Drill (host and clients): A3-D2a -- a client's take or use of a shared prop asks the host first. honest: one client "
    "takes and uses each family; race: two clients and the host contend for one prop (with --observer); depart: a "
    "client leaves holding a reservation and a newcomer takes its slot (with --rejoin); edge: the windows. Lines "
    "tagged [ACT-DRILL]; the host's PASS line ends a phase.")
