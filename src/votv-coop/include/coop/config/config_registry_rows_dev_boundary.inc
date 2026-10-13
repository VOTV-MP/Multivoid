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
    "bad_hand_family|keep_latest|chat_flood|world_latest",
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
    "its budget, still apply the last, and its laptop settle on it. Drill only.")
