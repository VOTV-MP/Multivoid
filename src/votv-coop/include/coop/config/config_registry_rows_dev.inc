// coop/config/config_registry_rows_dev.inc -- the registry row list's [dev] section, the last one, included at
// the end of config_registry_rows.inc, so each row keeps its place in every expansion of the list: the handle
// declarations, the index enum, the row table and the handle definitions. The entry forms are that file's.

// The [dev] section. The master switch: absent or garbage defaults to on, the granular
// switches deciding; only an explicit off forces dev features off.
CFG_FLAG(enabled, "enabled", "dev", true, nullptr,
    "Master switch for ALL dev/test features below: an explicit 0 disables them; absent or "
    "1 lets the granular switches decide.")
CFG_FLAG(devkeys, "devkeys", "dev", false, nullptr,
    "Enable the developer hotkeys.")
CFG_STRING(save, "save", "dev", "s_may2026", "VOTVCOOP_SAVE", false,
    "Test framework: the save slot name loaded at auto-start.")
CFG_FLAG(fresh_boot, "fresh_boot", "dev", false, "VOTVCOOP_FRESH",
    "Test framework: boot to a fresh new-game state instead of loading a save.")
CFG_FLAG(menu_autoload, "menu_autoload", "dev", false, "VOTVCOOP_MENU_AUTOLOAD",
    "Test framework: on a NATIVE menu launch, load the `save` slot from a worker once the play "
    "loop is already ticking -- the route a player takes, which no test scenario reached before, "
    "since the scenarios that load a world announce themselves as having booted into one. Says "
    "`rig: READY solo-world` when the world is up. Never on by default: it would load a save "
    "under a player who asked for the main menu.")
CFG_FLAG(static_2nd_player, "static_2nd_player", "dev", false, nullptr,
    "Spawn a static second-player puppet for camera/pose testing.")
CFG_FLAG(freecam, "freecam", "dev", false, nullptr,
    "Enable the freecam toggle.")
CFG_FLAG(spawn_menu_unlock, "spawn_menu_unlock", "dev", false, nullptr,
    "Unlock the native spawn menu while in a coop session.")
CFG_FLAG(desk_diag, "desk_diag", "dev", false, "VOTVCOOP_DESK_DIAG",
    "Diagnostic (BOTH peers): periodically log the signal desk's state, and the digests a day "
    "rollover or a world load rolls per machine (the dish hash codes, the coordinate towers, the "
    "servers' upgrades), to diff host against client. The env twin arms both peers for one run.")
CFG_INT(desk_diag_ms, "desk_diag_ms", "dev", 1000, 100, 60000, nullptr,
    "Interval in milliseconds for the desk_diag log.")
CFG_INT(vitals_keepalive_sec, "vitals_keepalive_sec", "dev", 0, 0, 86400, nullptr,
    "Reset player vitals every N seconds (0 = off; long unattended test runs).")
CFG_INT(hold_snapshot_sec, "hold_snapshot_sec", "dev", 0, 0, 600,
    "VOTVCOOP_DEV_HOLD_SNAPSHOT_SEC",
    "Host testing knob: defer each joiner's world bracket for N seconds before sending it "
    "(0 = off). Reproduces the one host-side wait nothing else can stage -- a joiner with a "
    "healthy link and a world, waiting on a bracket the host is holding. Under the joiner's "
    "budget the join still completes; over it, it fails naming the host's phase.")
CFG_INT(stall_world_stream_at_pct, "stall_world_stream_at_pct", "dev", 0, 0, 99,
    "VOTVCOOP_DEV_STALL_WORLD_STREAM_AT_PCT",
    "Host testing knob: stop feeding a joiner's world blob once N% of it has been handed to the "
    "transport (0 = off). The connection stays healthy and the host keeps beaconing its phase, "
    "so this is a download that STOPS rather than a link that dies -- the shape a fixed transfer "
    "cap used to answer by booting a fresh world and finishing the join in it. The joiner's "
    "download budget now names it instead.")
CFG_INT(bulk_queue_cap_ms, "bulk_queue_cap_ms", "dev", 2000, 0, 600000,
    "VOTVCOOP_DEV_BULK_QUEUE_CAP_MS",
    "How deep a queue a world-blob stream may stand behind, in milliseconds of the delivery "
    "measured on that link (0 = off, the byte rule alone). Bytes inside the transport's send "
    "buffer can be neither retracted nor reordered, so this is how long a cancelled transfer keeps "
    "spending the host's uplink. The 4 MiB buffer it replaces as the binding limit measured 5.6 s "
    "of queue on one policed joiner and 126 s on a slot losing a three-way race for one uplink, so "
    "2000 is 63x under the worst of that. 0 stages that before-picture on this same binary.")
CFG_FLAG(window_log, "window_log", "dev", false, "VOTVCOOP_WINDOW_LOG",
    "Diagnostic logging for window sync: each wipe sent, each value applied, and every window an index "
    "pass finds, one line each.")
CFG_FLAG(window_synth, "window_synth", "dev", false, "VOTVCOOP_WINDOW_SYNTH",
    "Test driver: once the joiner's world is ready, the host wipes a dirty window through its own "
    "cleanSponge in ten dabs each too small to send alone; their fall goes out when it passes the "
    "epsilon, and the client applies it.")
CFG_FLAG(window_stroke_log, "window_stroke_log", "dev", false, nullptr,
    "Diagnostic logging for the bay-window dab sync (every dab sent and received).")
CFG_FLAG(weather_observer_log, "weather_observer_log", "dev", false, nullptr,
    "Diagnostic logging for the weather sync observers.")
CFG_FLAG(weather_probe, "weather_probe", "dev", false, nullptr,
    "Diagnostic probe block for the weather subsystem.")
CFG_FLAG(meadow_selftest, "meadow_selftest", "dev", false, "VOTVCOOP_MEADOW_SELFTEST",
    "Autonomous selftest: the laptop signal database lane, every writer on the host and a client's row.")
CFG_FLAG(lid_drill, "lid_drill", "dev", false, "VOTVCOOP_LID_DRILL",
    "Autonomous drill: the portable PC's lid, opened on the host and closed on the client.")
CFG_FLAG(selftest_break_runtime_layer, "selftest_break_runtime_layer", "dev", false, "VOTVCOOP_SELFTEST_BREAK_RUNTIME_LAYER",
    "Red arm of the runtime-layer selftest: its first SetValue is skipped, so every check that reads the value it would have set must fail.")
CFG_FLAG(selftest_break_quoting, "selftest_break_quoting", "dev", false, "VOTVCOOP_SELFTEST_BREAK_QUOTING",
    "Red arm of the quoting selftest: its first check writes its line unquoted, so the ;-value reads back cut and the check must fail.")
CFG_FLAG(selftest_break_announce, "selftest_break_announce", "dev", false, "VOTVCOOP_SELFTEST_BREAK_ANNOUNCE",
    "Red arm of the announce selftest: its changed check compares a value with itself, so it must fail.")
CFG_FLAG(selftest_break_server_id, "selftest_break_server_id", "dev", false, "VOTVCOOP_SELFTEST_BREAK_SERVER_ID",
    "Red arm of the server_profile selftest: its first id case expects the wrong answer, so it must fail.")
CFG_INT(selftest_runtime_probe, "selftest_runtime_probe", "dev", 0, 0, 9, "VOTVCOOP_SELFTEST_RUNTIME_PROBE",
    "The row the runtime-layer selftest sets and reads; nothing else reads it.")
CFG_FLAG(selftest_break_commands, "selftest_break_commands", "dev", false, "VOTVCOOP_SELFTEST_BREAK_COMMANDS",
    "Red arm of the commands selftest: its first line case expects the wrong word count, so it must fail.")
CFG_FLAG(selftest_break_bug_report, "selftest_break_bug_report", "dev", false, "VOTVCOOP_SELFTEST_BREAK_BUG_REPORT",
    "Makes the bug-report selftest fail one check on purpose (a red arm).")
CFG_FLAG(selftest_break_atomic_file, "selftest_break_atomic_file", "dev", false, "VOTVCOOP_SELFTEST_BREAK_ATOMIC_FILE",
    "Makes the atomic-file selftest fail one check on purpose (a red arm).")
CFG_FLAG(selftest_break_stat_orders, "selftest_break_stat_orders", "dev", false, "VOTVCOOP_SELFTEST_BREAK_STAT_ORDERS",
    "Makes the stat-order wire selftest fail one check on purpose (a red arm).")
CFG_FLAG(selftest_break_build_trust, "selftest_break_build_trust", "dev", false, "VOTVCOOP_SELFTEST_BREAK_BUILD_TRUST",
    "Makes the build-trust selftest fail one check on purpose (a red arm).")
CFG_FLAG(build_trust_test_key, "build_trust_test_key", "dev", false, "VOTVCOOP_BUILD_TRUST_TEST_KEY",
    "Trust the build-trust selftest's TEST key as a release key on this peer, so a rig build signed with it reads "
    "as official here. Never set on a player's install.")
CFG_FLAG(build_claim_foreign, "build_claim_foreign", "dev", false, "VOTVCOOP_BUILD_CLAIM_FOREIGN",
    "Send this peer's Join with its build hash's first byte flipped, so a host reads it as another build (an arm of "
    "the build-trust drill).")
CFG_FLAG(selftest_break_identity, "selftest_break_identity", "dev", false, "VOTVCOOP_SELFTEST_BREAK_IDENTITY",
    "Makes the identity selftest's key-write case fail on purpose (a red arm).")
CFG_ENUM(subject_drill, "subject_drill", "dev", "off", "off|insert|own|carry|carryred", "VOTVCOOP_SUBJECT_DRILL",
    "Drill (BOTH peers), a remote action never takes the local player as its subject. insert: before any "
    "client's world is ready the host spawns two drives; the joined client walks to the nearer, grabs it "
    "with the use key's chain and holds it, and once the host has seated the other in the desk's play "
    "slot the client's grab must still hold. own: the client grabs the host's one drive the same way and "
    "seats it in its own play slot, and its grab must end there, as single player's does. carry, on the saved "
    "world: the host empties the desk's play slot, the client carries its nearest drive to the play port by "
    "hand, latches its own slot and holds the drive in its port, and the host's port must refuse the carried "
    "mirror, which the host's '[subject_drill] host DONE' line judges; carryred is its control. Run insert and "
    "own on a fresh host world; the client's '[subject_drill] client DONE' line ends them.")
CFG_FLAG(deck_selftest, "deck_selftest", "dev", false, nullptr,
    "Autonomous selftest: tape deck playback sync.")
CFG_FLAG(drive_selftest, "drive_selftest", "dev", false, "VOTVCOOP_DRIVE_SELFTEST",
    "Autonomous selftest: drive rack digest instrument.")
CFG_FLAG(desk_snd_selftest, "desk_snd_selftest", "dev", false, nullptr,
    "Autonomous selftest: desk sound mirror.")
CFG_FLAG(container_selftest, "container_selftest", "dev", false, "VOTVCOOP_CONTAINER_SELFTEST",
    "Autonomous selftest: container contents sync. It MUTATES the world -- it extracts an item on"
    " each peer -- so it is armed per run from the environment, like the hand-drop one.")
CFG_FLAG(container_opener_probe, "container_opener_probe", "dev", false, "VOTVCOOP_CONTAINER_OPENER_PROBE",
    "Probe (host): once each client's world is up and it has a body, every drone, dronesack and ATV with "
    "the container it opens, the lookups both ways that must name the pair, and each client's reach to "
    "both, again when a client stands at an opener; a verdict per pass. Read-only.")
CFG_FLAG(container_view_drill, "container_view_drill", "dev", false, "VOTVCOOP_CONTAINER_VIEW_DRILL",
    "Drill (client): walks to the ATV, opens its container through the game's own openPropInv, then walks "
    "back; the view must stay open while the ATV is within reach and close on the tick it leaves it. Run it "
    "with join_at_host=1. Lines tagged [CVIEW-DRILL]; the client's DONE line, carrying the verdict, ends it.")
CFG_FLAG(hand_drop_selftest, "hand_drop_selftest", "dev", false, "VOTVCOOP_HAND_DROP_SELFTEST",
    "Autonomous selftest: a prop through a player's hotbar HAND and back out -- pick it up with "
    "the game's own verb, drop it, first with the host holding and then with the client, twice "
    "each. The verdict is the watching peer's census of the keyed props around the holder before "
    "and after: a prop that does not come back is the release the other peer never received. It "
    "MUTATES the world, so it is armed per run from the environment.")
CFG_FLAG(run_and_wait_selftest, "run_and_wait_selftest", "dev", false, "VOTVCOOP_RUN_AND_WAIT_SELFTEST",
    "Autonomous selftest: the one game-thread wait driven through its five endings -- an answer, no "
    "answer, a fault, a C++ throw and a call on the game thread itself -- once per process. Its faults "
    "are provoked on purpose and logged as ERRORs its verdict names, so arm it for one run.")
CFG_FLAG(world_singleton_parity, "world_singleton_parity", "dev", false, "VOTVCOOP_WORLD_SINGLETON_PARITY",
    "Probe: once per world, the menu's included, the object index's loaded-class lookup and the world "
    "singleton checked against the whole-array walks they replace, one row per name and a verdict. "
    "Read-only; the walks it makes for the comparison are its own.")
CFG_FLAG(function_lookup_parity, "function_lookup_parity", "dev", false, "VOTVCOOP_FUNCTION_LOOKUP_PARITY",
    "Probe: once per world, every function a class owns, found by one walk of the object array, "
    "looked up by its class and name through reflection::FindFunction's own-list read; mismatches "
    "named, and a verdict. Read-only; the pass renders every function's name, a visible hitch.")
CFG_FLAG(end_play_probe, "end_play_probe", "dev", false, "VOTVCOOP_END_PLAY_PROBE",
    "Probe: every actor's end of play counted by reason, and each destroyed one matched to the "
    "K2_DestroyActor call it ran inside; the ends with no such call, and the calls with no end, named "
    "by class every 30 s. Read-only.")
CFG_FLAG(death_seam_census, "death_seam_census", "dev", false, "VOTVCOOP_DEATH_SEAM_CENSUS",
    "Probe: every element end the death seam announces, one [DEATH] line each -- the element's type "
    "and id, mirror or local, destroyed or streamed out, and its class -- to pair with the lanes' own "
    "death detections. Read-only.")
CFG_FLAG(damage_probe, "damage_probe", "dev", false, "VOTVCOOP_DAMAGE_PROBE",
    "Probe (BOTH peers): every call of the player's damage verb, one [DMG] line each -- this machine's "
    "own player or a peer's puppet, the amount, the source's class and the calling Blueprint function. "
    "Read-only.")
CFG_FLAG(player_damage_no_refusal, "player_damage_no_refusal", "dev", false, "VOTVCOOP_PLAYER_DAMAGE_NO_REFUSAL",
    "Developer: never refuse damage or ignite on a peer's puppet (the dmghazard probe's red).")
CFG_FLAG(event_drill, "event_drill", "dev", false, "VOTVCOOP_EVENT_DRILL",
    "Drill (BOTH peers): the host fires the meteor shower before the client's world is up (the join "
    "snapshot carries it) and then through the scheduler itself (the clock set to day 2 00:18), then "
    "solar, arirGraff_0 and enasus by the dev fire; the client's DONE line counts the showers and the "
    "fires it replayed. MUTATES the world (the clock, the events), so it is armed per run.")
CFG_FLAG(egg_drill, "egg_drill", "dev", false, "VOTVCOOP_EGG_DRILL",
    "Drill (BOTH peers): once the client's world is up the host fires eggvasion (121 eggs, the "
    "npc lane's largest population); the client says how many NPC mirrors it holds every 5 s and "
    "DONE 30 s after more than 100 exist, so the run's [perf] lines show what the eggs cost it. "
    "MUTATES the world (an event), so it is armed per run.")
CFG_ENUM(command_drill, "command_drill", "dev", "off", "off|on|red|grant|grantred|hostdeny", "VOTVCOOP_COMMAND_DRILL",
    "Commands drill: once its world is ready, this peer submits /help and an unknown command, then a "
    "line one byte too long for a request (and, on the host, one of exactly the limit), then (a "
    "client) six unknown commands at once, and checks the replies in order. red expects a wrong first "
    "line, so it must fail. grant / grantred: a client runs a command and reports whether the host's "
    "permissions allowed it (grant: one the rig's fixture grants; grantred: one it does not); hostdeny: "
    "the host runs a command its own permission file denies it and reports the refusal.")
CFG_ENUM(mv_drill, "mv_drill", "dev", "off", "off|on|red", "VOTVCOOP_MV_DRILL",
    "Drill (BOTH peers): /mv end to end -- on runs the script, red expects a wrong reply at step 5.1.")
CFG_ENUM(stats_probe, "stats_probe", "dev", "off", "off|on|red", "VOTVCOOP_STATS_PROBE",
    "Probe (BOTH peers): every player stat read, written and restored inside one frame, and the game's "
    "status effects added, listed and removed -- red expects a wrong Health read-back and two bloodLoss entries.")
CFG_ENUM(stat_order_drill, "stat_order_drill", "dev", "off", "off|on|red|redjoin", "VOTVCOOP_STAT_ORDER_DRILL",
    "Drill (the HOST): order and query a client's stats and effects and check every answer -- red expects a wrong "
    "answer, redjoin the host's effect on the joiner.")
CFG_ENUM(bug_report_drill, "bug_report_drill", "dev", "off", "off|on|red", "VOTVCOOP_BUG_REPORT_DRILL",
    "The bug-report drill: plants values, saves a report, opens it and judges it (red: plants an address unmarked).")
CFG_ENUM(grants_drill, "grants_drill", "dev", "off", "off|grant|none|hostdeny", "VOTVCOOP_GRANTS_DRILL",
    "The local grants drill: checks this machine's dev grants against an arm's expectation.")
CFG_ENUM(grants_drill_expect, "grants_drill_expect", "dev", "same", "same|grant|none|hostdeny",
    "VOTVCOOP_GRANTS_DRILL_EXPECT", "Judges the grants drill against another arm's expectation (a red arm).")
CFG_ENUM(chat_drill, "chat_drill", "dev", "off", "off|history|seed|i18n", "VOTVCOOP_CHAT_DRILL",
    "Developer: chat checks judged in memory -- history (the host), seed (the second joiner), i18n "
    "(every peer); each prints [CHAT-DRILL] PASS, FAIL or ABORT.")
CFG_FLAG(chat_drill_bad_nick, "chat_drill_bad_nick", "dev", false, "VOTVCOOP_CHAT_DRILL_BAD_NICK",
    "Developer: the chat drill announces a wrong nickname (the i18n red).")
CFG_FLAG(chat_no_retain, "chat_no_retain", "dev", false, "VOTVCOOP_CHAT_NO_RETAIN",
    "Developer: never keep a chat line in history (the chat drill's history red).")
CFG_FLAG(chat_seed_suppress, "chat_seed_suppress", "dev", false, "VOTVCOOP_CHAT_SEED_SUPPRESS",
    "Developer: send a joiner no chat history (the seed red).")
CFG_ENUM(pause_quit_drill, "pause_quit_drill", "dev", "off", "off|client|host",
    "VOTVCOOP_PAUSE_QUIT_DRILL",
    "Developer: the named peer opens the pause menu and presses Main menu, as a player does, once it "
    "is in the session; prints [PAUSE-QUIT] lines.")
CFG_FLAG(quit_decision_red, "quit_decision_red", "dev", false, "VOTVCOOP_QUIT_DECISION_RED",
    "Developer: the quit decision does NOT end the session (the pause-quit drill's red: the menu "
    "world's destroys go out).")
CFG_FLAG(wire_census, "wire_census", "dev", false, "VOTVCOOP_WIRE_CENSUS",
    "Developer: log the wire census every second (the wirewindow probe).")
CFG_FLAG(ban_drill, "ban_drill", "dev", false, "VOTVCOOP_BAN_DRILL",
    "Ban drill (host only): when a client has finished joining, ban slot 1 once through the "
    "moderation verb and report whether the ban was stored and the seat closed. It writes the "
    "host's bans.json: run it under lane.py prove, which restores the server folders, never in a "
    "plain smoke.")
CFG_FLAG(ban_drill_stale_token, "ban_drill_stale_token", "dev", false, "VOTVCOOP_BAN_DRILL_STALE_TOKEN",
    "Ban drill's red arm: aim the ban with a generation one past the slot's, so the token check "
    "refuses it and the drill reports FAIL.")
CFG_ENUM(settings_drill, "settings_drill", "dev", "off", "off|nameplate|nickcolor|flags|scale|font|voicemode|voicevolume|server|serverjoin|serverred|servercmd|servercmdred|servercmdspellred|red", "VOTVCOOP_SETTINGS_DRILL",
    "Developer: a settings step after the session starts, one token per step; the server steps (server, "
    "serverjoin, serverred, servercmd, servercmdred, servercmdspellred) exercise the host's voice range, each red by a designed failure. "
    "nameplate: the "
    "nameplate row to hidden (the client logs both announces). nickcolor: the nick colour to the "
    "per-surface default. flags: the chat peer-actions flag off and the netstats flag on. "
    "scale: ui.scale to 1.50. font: the first font role's family. "
    "voicemode: voice.mode to activation. voicevolume: voice.volume to 0.50. "
    "server: after the client's world is up the host sets voice.distance_cm to 6000, then resets it; "
    "the client's log shows each value arrive. serverjoin: the host sets it before any client "
    "connects, so the joiner's snapshot carries it, and resets after the joiner's world is up. "
    "serverred: server with the set skipped, so the host's probe prints its FAIL line. "
    "servercmd: the server step through /set and /reset, with a respell of the set value between them; "
    "servercmdred: a /set the command refuses (a local row named). "
    "servercmdspellred: as servercmd, its respell step a real change (6001), so the client announces it "
    "where servercmd forbids it. "
    "red: the nameplate step with its set skipped, so the probe's FAIL line prints.")
CFG_ENUM(mannequin_drill, "mannequin_drill", "dev", "off", "off|spawn|red", "VOTVCOOP_MANNEQUIN_DRILL",
    "Drill (BOTH peers), the walking mannequin's baseline. spawn: once the client's join is over the host spawns one native walker (it opens doors, is saved with the world and returns angry after a load; it crosses to a client as a prop mirror with a brain of its own) and both peers log what they see of it once a second, the host for 20 samples. red: the same without the spawn call, so the sampler must report that no walker appeared: the drill's control.")
CFG_FLAG(floppy_selftest, "floppy_selftest", "dev", false, "VOTVCOOP_FLOPPY_SELFTEST",
    "Autonomous selftest: the disc-into-server media transfer -- a client insert the host tries to "
    "eject, the mirror of that, and a client insert the same client ejects. It MUTATES the world "
    "(it inserts, ejects, and seeds discs when the world has none), so arm it from the environment "
    "for one run rather than leaving it here. Set it on BOTH peers; each drives its own role.")
CFG_FLAG(hookdrag_selftest, "hookdrag_selftest", "dev", false, "VOTVCOOP_HOOKDRAG_SELFTEST",
    "Autonomous selftest: a prop dragged by a hook. The planting peer plants a hook into the "
    "nearest keyed physics prop, walks out and back so the constraint drags it, then drops the "
    "hook; both peers log the prop's position on one clock. It MUTATES the world, so arm it from "
    "the environment for one run, on BOTH peers.")
CFG_ENUM(hookdrag_role, "hookdrag_role", "dev", "host", "host|client|both", "VOTVCOOP_HOOKDRAG_ROLE",
    "Which peer plants the hook in the hookdrag selftest: the host (the driven-prop channel alone), "
    "the client (the host ties the client's hook on its mirror), or both into the same prop (two "
    "host constraints on one body). Set the same value on BOTH peers.")
CFG_FLAG(inventory_selftest, "inventory_selftest", "dev", false, nullptr,
    "Autonomous selftest: player inventory sync.")
CFG_FLAG(starterkit_test, "starterkit_test", "dev", false, nullptr,
    "Autonomous selftest: starter kit issuance.")
CFG_FLAG(reseed_orphan_selftest, "reseed_orphan_selftest", "dev", false, nullptr,
    "Autonomous selftest: save-identity reseed/orphan handling.")
CFG_FLAG(roster_token_selftest, "roster_token_selftest", "dev", false, nullptr,
    "Autonomous selftest (HOST): when a peer slot changes hands, prove a token captured from the "
    "previous occupant is refused -- the ban that must never land on their successor.")
CFG_FLAG(roster_drop_empty_rows, "roster_drop_empty_rows", "dev", false, nullptr,
    "Fault injection (CLIENT): discard every inbound roster row that says a slot is EMPTY, so a "
    "departure can only be learned from the successor's row. Proves absence is never needed.")
CFG_FLAG(save_wait_probe, "save_wait_probe", "dev", false, "VOTVCOOP_SAVE_WAIT_PROBE",
    "Probe: the host holds each save request as if its world were still loading, for a second after it is in.")
CFG_FLAG(pile_delta_probe, "pile_delta_probe", "dev", false, nullptr,
    "Diagnostic probe: trash pile delta events.")
CFG_FLAG(garbage_pickup_probe, "garbage_pickup_probe", "dev", false, nullptr,
    "Diagnostic probe: garbage pickup path.")
CFG_FLAG(world_actor_trace, "world_actor_trace", "dev", false, nullptr,
    "Diagnostic trace: the client-side world-actor mirror drive, once a second per mirror. Costs "
    "two extra location reads per mirror per second while on.")
CFG_FLAG(grime_log, "grime_log", "dev", false, "VOTVCOOP_GRIME_LOG",
    "Diagnostic logging for grime/cleaning sync: each fall sent, each value applied, and every decal an index pass finds, one line each.")
CFG_FLAG(grime_drill, "grime_drill", "dev", false, "VOTVCOOP_GRIME_DRILL",
    "Drill (BOTH peers): each cleans one of the same two rain-proof decals partway, then below zero, in the "
    "rain's clean call form, and ends when its copy of the other's decal reads zero and still stands. Lines "
    "tagged [GRIME-DRILL]; the client's DONE line ends it.")
CFG_FLAG(interactable_log, "interactable_log", "dev", false, nullptr,
    "Diagnostic logging for the interactable channel.")
CFG_FLAG(channel_shadow_probe, "channel_shadow_probe", "dev", false, "VOTVCOOP_CHANNEL_SHADOW_PROBE",
    "Diagnostic probe beside the keyed interactable lanes, which send at their verbs: polls each indexed "
    "device every tick and says, as a SHADOW MISS, a change the lane neither sent nor applied. Sends nothing.")
CFG_FLAG(client_model_probe, "client_model_probe", "dev", false, nullptr,
    "Diagnostic probe: client-side model/skin state.")
CFG_FLAG(delivery_census, "delivery_census", "dev", false, nullptr,
    "Diagnostic census: delivery spawn events.")
CFG_FLAG(store_table_probe, "store_table_probe", "dev", false, nullptr,
    "Diagnostic census: which mechanism can read a list_store row (one-shot).")
CFG_FLAG(order_selftest, "order_selftest", "dev", false, "VOTVCOOP_ORDER_SELFTEST",
    "Selftest (BOTH peers): once a client's world is up it places a real shop order through the game's own "
    "shop, so host-side pricing is exercised; once a client's world is ready the host makes the day cycle's "
    "own daily order, a world event's, whose items the queue mirror carries by class. The client's DONE line "
    "says whether that order reached its queue with every item built. Lines tagged [order_selftest].")
CFG_FLAG(order_probe, "order_probe", "dev", false, "VOTVCOOP_ORDER_PROBE",
    "Probe (BOTH peers): logs each write of the delivery order queue (makeAnOrder with its caller, addOrderCart, removeOrderCart), the drone's sendShop, checkOrders and compileOrder, and the day cycle's hourly call that makes the daily order. Lines tagged [ORDER-PROBE].")
CFG_FLAG(flashlight_log, "flashlight_log", "dev", false, nullptr,
    "Diagnostic logging for flashlight/item-activate sync.")
CFG_FLAG(kerfur_census, "kerfur_census", "dev", false, nullptr,
    "Diagnostic census: kerfur units and their identity records.")
CFG_FLAG(object_overlay, "object_overlay", "dev", false, nullptr,
    "Debug overlay: object/entity markers.")
CFG_FLAG(ragdoll_bone_overlay, "ragdoll_bone_overlay", "dev", false, nullptr,
    "Debug overlay: ragdoll bone positions.")
CFG_FLAG(join_window_pos_trace, "join_window_pos_trace", "dev", false, nullptr,
    "Diagnostic trace: entity positions during the join window.")
CFG_FLAG(puppet_head_probe, "puppet_head_probe", "dev", false, nullptr,
    "Diagnostic probe: puppet head/aim pose.")
CFG_FLAG(atv_probe, "atv_probe", "dev", false, nullptr,
    "Diagnostic probe: the ATV rig baseline (4-body pose, suspension travel, vitals).")
CFG_FLAG(atv_probe_sit, "atv_probe_sit", "dev", false, nullptr,
    "ATV probe DRIVE arm (ONE peer, one shot): seat the local player, release the brake and hold the throttle so the ATV is authored AND moving. Set it on one peer only.")
CFG_FLAG(atv_corrector, "atv_corrector", "dev", true, "VOTVCOOP_ATV_CORRECTOR",
    "ATV pose corrector, ON by default. Set 0 to leave a mirrored ATV entirely alone -- the "
    "CONTROL arm for 'does the mirror settle wrong on its own, or do we settle it wrong'. The "
    "mirror still simulates and still receives state; only the position/velocity correction is "
    "withheld, so the two arms differ in exactly one thing.")
CFG_FLAG(native_pile_inert_probe, "native_pile_inert_probe", "dev", false, nullptr,
    "Diagnostic probe: native pile inertness on clients.")
CFG_FLAG(spawn_order_probe, "spawn_order_probe", "dev", false, nullptr,
    "Diagnostic probe: actor spawn ordering.")
CFG_FLAG(food_clock_probe, "food_clock_probe", "dev", false, "VOTVCOOP_FOOD_CLOCK_PROBE",
    "Diagnostic probe (BOTH peers): what a food's save record does to its own state on arrival. "
    "Aprop_food_C::loadData catches the food up against a timestamp the SENDER wrote with its own "
    "world clock, so this prints the two clocks, their difference, and how far the temperature and "
    "ripeness moved because of it.")
CFG_FLAG(lookat_churn_probe, "lookat_churn_probe", "dev", false, "VOTVCOOP_LOOKAT_CHURN_PROBE",
    "Diagnostic probe (BOTH peers): how often the look-at set the interaction UI is rebuilt from "
    "changes while the aim holds still, and which of its five fields moves. A held aim should "
    "change nothing; every change past the first is one visible reset of the hovertext and the "
    "action list, and the interval between them names the timer behind it.")
CFG_FLAG(rollover_watch, "rollover_watch", "dev", false, "VOTVCOOP_ROLLOVER_WATCH",
    "Diagnostic probe (BOTH peers): the world clock's day rollover. Watches the roll's own verbs "
    "(the dish hash codes, the task shuffle, the Bad Sun), the minute and hour pulses and every "
    "Blueprint bound to them, and prints each peer's day number and a digest of the 24 hash codes "
    "whenever the day changes. Read-only; it never refuses a call.")
CFG_FLAG(load_reroll_watch, "load_reroll_watch", "dev", false, "VOTVCOOP_LOAD_REROLL_WATCH",
    "Diagnostic probe (BOTH peers): what a world load rolls again. Watches the server-upgrade "
    "spawner and the coordinate towers' scramble from before a joining client's world exists, "
    "since both run inside the load, and logs the first calls with their callers. It holds the "
    "script gate on while set, with or without a session, so every lane's gate watch also runs "
    "outside a session for that run; read-only otherwise.")
CFG_ENUM(midnight_drill, "midnight_drill", "dev", "off", "off|awake|asleep|cheat|mode5|malformed|joinwindow",
    "VOTVCOOP_MIDNIGHT_DRILL",
    "Drill (BOTH peers, with rollover_watch), once a client's join is over. awake and asleep: the "
    "host sets its clock just short of midnight; awake: both peers stay up; asleep: both go to bed "
    "first, so the midnight falls inside the shared sleep's fast-forward. cheat: the host leaves its "
    "clock alone and the client writes a day onto its own, three times, as the cheat menu's day "
    "button does. mode5: each peer spawns game mode 5's master, which resets the day every second, "
    "and the client says whether its own reset kept running. malformed: the host puts three clock "
    "samples whose day is not a number, and the client's log says where each was refused. joinwindow: "
    "the host sets its clock just short of midnight once it has taken a joiner's world, so the midnight "
    "falls inside the join, and both peers print their dishes' hash digest; equal is a pass. Run it on a "
    "fresh host world; every refused step is logged and ends the arm as invalid.")
CFG_ENUM(world_roll_drill, "world_roll_drill", "dev", "off",
    "off|eye_host|eye_client|eye_join|jelly_host|jelly_client|jelly_join|redsky_host|redsky_client|redsky_join",
    "VOTVCOOP_WORLD_ROLL_DRILL",
    "Drill (BOTH peers), the day's world rolls: each a host roll that must cross and a client's own that "
    "must be refused. eye_host: once a client's join is over the host's sky runs setEye(true), as its noon "
    "roll would, and the client's copy must show the eye within 20 s; eye_client: the joined client runs "
    "its own setEye(true) and the gate must refuse it, its copy's eye unchanged; eye_join: the host's sky "
    "runs setEye(true) before any client's world is ready, and the joiner's copy must show it. jelly_host: "
    "once a client's join is over the host's jellyfish path spawns, as its 18:00 roll would, and the "
    "client's copy must show the seven fish as mirrors within 20 s and see them leave with the host's run; "
    "jelly_client: the joined client's own spawn must leave its path's list as it was; jelly_join: the "
    "host's run is under way before any client's world is ready. redsky_host: once a client's join is over "
    "the host's gamemode runs its noon toggle twice, starting a red sky and ending it, and the client's "
    "copy must see both edges within 20 s; redsky_client: the joined client's own toggle must be refused, "
    "its sky unchanged; redsky_join: the host's red sky stands before any client's world is ready, the "
    "joiner's copy must show it within 20 s, and then the joiner's own toggle, as its noon would run it with "
    "a red sky live, must be refused, its sky still red. Run it on a fresh host world; every refused step is "
    "logged and ends the arm as invalid.")
CFG_ENUM(fireext_drill, "fireext_drill", "dev", "off", "off|carry|short|client|join",
    "VOTVCOOP_FIREEXT_DRILL",
    "Drill (BOTH peers): a wall-mounted fire extinguisher taken off through the use key's own grab "
    "chain. carry: the host carries it a few metres once a client's join is over, and the client runs "
    "the laptop exit's setPropProps on its carried copy once, which the drive must re-latch. short: "
    "the host lets go the moment it holds it, then re-sends the closed hold's pose for a moment, which "
    "every receiver must drop. client: the client carries it and the host watches. join: the host "
    "takes it off while a joiner is still loading. Both peers print every extinguisher they have and "
    "every move of one.")
CFG_ENUM(pry_drill, "pry_drill", "dev", "off", "off|host|client|join",
    "VOTVCOOP_PRY_DRILL",
    "Drill (BOTH peers): a stuck pryable (a sign, a torch holder) pried off its wall by its own "
    "crowbarOpen, with no crowbar. host: the host pries once a client's join is over, after the client "
    "runs its copy's hand unstick, which a pryable refuses with its 'Tool required' hint. client: the "
    "client pries and the host watches. join: the host pries while a joiner is still loading. Both peers "
    "print every pryable they have and every change of one.")
CFG_FLAG(recycled_slot_drill, "recycled_slot_drill", "dev", false, "VOTVCOOP_RECYCLED_SLOT_DRILL",
    "Drill (either peer): once the local pawn and the object index are up, stage an object slot the "
    "engine recycled on the pawn's root component -- a queued birth naming the slot's old class, and a "
    "stale listing -- then dispatch the actor location read on the component, whose read faults inside "
    "the engine. One verdict line per half, then DONE.")
CFG_FLAG(kerfur_menu_drill, "kerfur_menu_drill", "dev", false, "VOTVCOOP_KERFUR_MENU_DRILL",
    "Drill (HOST): once a client's join is over, spawn a kerfur, turn it off "
    "by its verb, then turn the prop back on through the menu event a press dispatches, where the "
    "verb runs nested inside a watched body. Prints the kerfur form assembler's order counters.")
CFG_FLAG(kerfur_convert_drill, "kerfur_convert_drill", "dev", false, "VOTVCOOP_KERFUR_CONVERT_DRILL",
    "Drill (BOTH peers): the host keeps every kerfur carrying a disc and spawns one more; the client "
    "turns one off by its verb and back on by the prop's turn-on action while a disc of its own lies 2 m "
    "away, then says whether both conversions applied, each form stood when its own call returned, the "
    "carried disc arrived once and its own disc survived. '[KERFUR-DRILL] client DONE' ends it.")
CFG_FLAG(kerfus_drill, "kerfus_drill", "dev", false, "VOTVCOOP_KERFUS_DRILL",
    "Drill (BOTH peers): the host stays put; the client presses the plain kerfur (the Kerfus) on, walks to "
    "a pile the director keeps 10 m from the host, and the Kerfus must settle by the client, not the host; "
    "then the host possesses it, the client walks back once the haunting's energy pin shows, and the Kerfus "
    "must go off away from where it settled. The rig client joins at the base. '[KERFUS-DRILL] client DONE "
    "PASS|FAIL' ends it; '[KERFUS-DRILL] INVALID' is a step that could not run.")
CFG_FLAG(kerfus_throw_drill, "kerfus_throw_drill", "dev", false, "VOTVCOOP_KERFUS_THROW_DRILL",
    "Drill (BOTH peers): the host grabs the plain kerfur (the Kerfus) and lets it go or throws it, three times "
    "off and three on, and grabs it once more; then the client grabs it twice. Each peer says whether its Kerfus "
    "stayed one body: the copy where the host's hand has it, its centre of mass on the body, its welded "
    "navigation pawn where the body carries it. The rig client joins at the base. '[KERFUS-THROW] host DONE "
    "PASS|FAIL' ends it; '[KERFUS-THROW] INVALID' is a step that could not run.")
CFG_FLAG(kerfur_serve_drill, "kerfur_serve_drill", "dev", false, "VOTVCOOP_KERFUR_SERVE_DRILL",
    "Drill (BOTH peers): the host spawns a kerfur Omega, takes a red disc in hand and stays put; the client's "
    "first command is get_reports, and the host's disc must stay in its hand with the Omega holding none, back "
    "in the follow state; the client then walks to a pile the director keeps 10 m from the host and commands "
    "follow, and the Omega must settle by it in the follow state (the client's 'leg A' line); then the client "
    "commands idle, the host runs the kill verb, and the murderous Omega must go for the host. '[KERFUR-SERVE] "
    "host DONE' ends it; '[KERFUR-SERVE] INVALID' is a step that could not run.")
CFG_FLAG(door_drill, "door_drill", "dev", false, "VOTVCOOP_DOOR_DRILL",
    "Drill (BOTH peers): the client walks with the bot director to the nearest door its navmesh "
    "reaches and uses it as a player does -- three presses, each followed by a stay in its sensor until "
    "the door's own next check (stepped in from outside it mid-swing, from outside after the swing, "
    "from the approach point) and a walk away, then a weapon's hits; each peer logs every change in "
    "what each door's own sensor holds, puppets and local players apart, and its open flag, and the "
    "walked door's own sensor events. Lines tagged [DOOR-DRILL].")
CFG_FLAG(door_drill_host, "door_drill_host", "dev", false, "VOTVCOOP_DOOR_DRILL_HOST",
    "With door_drill: the walker runs on the HOST instead of the client, so its own player in its own "
    "door is single player's reading of the same phases -- the control for the client's run.")
CFG_FLAG(blackout_drill, "blackout_drill", "dev", false, "VOTVCOOP_BLACKOUT_DRILL",
    "Drill (BOTH peers): once a client's world is ready the host lights the power panel's light groups and "
    "shuts its blackout doors, then fires the solar event, cutting every breaker on both peers (the client "
    "replays it), turning the groups off and opening the doors on their lanes; each peer's DONE comes once "
    "its own copy reads every breaker cut, every group off and every blackout door open. For the lanes' dev "
    "probe (channel_shadow_probe=1) to watch a blackout that changes what they carry. Lines tagged "
    "[BLACKOUT-DRILL]; the client's DONE line ends it. MUTATES the world (the power), so it is armed per run.")
CFG_STRING(blackout_drill_door, "blackout_drill_door", "dev", "", "VOTVCOOP_BLACKOUT_DRILL_DOOR", false,
    "With blackout_drill: the object name of a door the blackout does not open (e.g. door11_207, the base "
    "bedroom's). The host shuts it before the fire and makes it inactive once its breakers read cut, if the "
    "blackout left it active; once the client's copy reads the blackout it walks to that door and presses "
    "it, which runs on the host, and the door must open by hand, as single player opens an inactive door "
    "while the lights' power is off. Empty skips the leg.")
CFG_FLAG(blackout_drill_control, "blackout_drill_control", "dev", false, "VOTVCOOP_BLACKOUT_DRILL_CONTROL",
    "With blackout_drill and blackout_drill_door: the control for the door leg. No fire: the host makes the "
    "named door inactive with the power on, and the client's press, run on the host, must leave it shut, as "
    "single player's own clause does while the lights have power.")
CFG_FLAG(light_drill, "light_drill", "dev", false, "VOTVCOOP_LIGHT_DRILL",
    "Drill (BOTH peers): the light switch with the lowest key in the switch lane whose group the group "
    "lane names and whose breaker is on, pressed by the client through the switch's own use() -- its "
    "own copy of the group must not move -- and back by the host whenever a client's press moved its "
    "group; each peer logs every change of the group. Lines tagged [LIGHT-DRILL]; the client's DONE "
    "line ends it.")
CFG_FLAG(keypad_drill, "keypad_drill", "dev", false, "VOTVCOOP_KEYPAD_DRILL",
    "Drill (BOTH peers): the client walks to the nearest keypad that gates a door and types the "
    "password through the numpad, a numpad cancel after two digits and a wrong code on the keys, each "
    "ended by its own copy landing on the host's verdict with the gated door's power; the host logs "
    "every keypad's changes. Lines tagged [KEYPAD-DRILL]; the client's DONE line ends it.")
CFG_STRING(keypad_drill_blackout, "keypad_drill_blackout", "dev", "", "VOTVCOOP_KEYPAD_DRILL_BLACKOUT", false,
    "With keypad_drill: restore -- the host breaks a generator as the client's world is ready, the client walks "
    "only once its copy reads the blackout, the host restores the power (the repair, the light's breaker on) "
    "once the client stands at a keypad, and the client types only once its copy reads the power back, its keys "
    "pressed through the click that needs the light's power; stale -- the same, then the client's own light "
    "power written off, as the old power lane left a client, which its key legs must FAIL.")
CFG_FLAG(physmods_drill, "physmods_drill", "dev", false, "VOTVCOOP_PHYSMODS_DRILL",
    "Drill (BOTH peers), on an empty desk: the host plugs a module through the desk's plugInModule, the "
    "client plugs a second of that type into another slot, the host unplugs its own through the desk's E "
    "press, then the client unplugs its own the same way and plugs one of another type; the host's DONE "
    "line says whether its array holds that one module alone. A joiner whose desk already holds modules at "
    "its world-ready (a rejoin into the drill's world) plugs nothing and says what it loaded. Lines tagged "
    "[PHYSMODS-DRILL].")
CFG_STRING(atv_spawn_drill, "atv_spawn_drill", "dev", "", "VOTVCOOP_ATV_SPAWN_DRILL", false,
    "Drill (BOTH peers): a runtime ATV spawned on the host reaches each client once. run -- spawned once the client's "
    "world is ready; join -- while it is still loading (the world-ready replay); park -- as run, with "
    "atv_park_first_spawn=1 on the client. The client's DONE line ends it.")
CFG_FLAG(atv_park_first_spawn, "atv_park_first_spawn", "dev", false, "VOTVCOOP_ATV_PARK_FIRST_SPAWN",
    "Developer: the first runtime ATV spawn this client receives is parked as if its world were not ready (the ATV "
    "spawn drill's park arm).")
CFG_STRING(fall_drill, "fall_drill", "dev", "", "VOTVCOOP_FALL_DRILL", false,
    "Drill (BOTH peers): a host birth that falls lands on the client where it lands on the host. run -- the host "
    "drops a drive from 4 m once the client's world is ready; the client's DONE line ends it.")
CFG_FLAG(prop_fall_no_coast, "prop_fall_no_coast", "dev", false, "VOTVCOOP_PROP_FALL_NO_COAST",
    "Developer: a falling host birth is not streamed to the clients (the fall drill's red).")
CFG_STRING(sack_drill, "sack_drill", "dev", "", "VOTVCOOP_SACK_DRILL", false,
    "Drill (BOTH peers): a client's point sack is paid by the host, once. run -- the host spawns a sack before the "
    "client's puppet once its world is ready; the client aims and presses it; the host's DONE line ends it.")
CFG_FLAG(point_sack_client_runs, "point_sack_client_runs", "dev", false, "VOTVCOOP_POINT_SACK_CLIENT_RUNS",
    "Developer: a client's point sack runs the game's own body there instead of asking the host (the sack drill's red).")
CFG_FLAG(drone_call_drill, "drone_call_drill", "dev", false, "VOTVCOOP_DRONE_CALL_DRILL",
    "Drill (BOTH peers): the client walks to the garage's drone console, opens its lid if it is shut, aims "
    "at its keyboard and presses it through the player's own E dispatch; the press goes to the host, which "
    "presses its console for the client. A sack the first press brought is followed by a second press, and "
    "the client's DONE line says whether the drone flew on its copy; the host logs its drone's changes. "
    "Lines tagged [DRONE-CALL-DRILL].")
CFG_STRING(sat_console_drill, "sat_console_drill", "dev", "", "VOTVCOOP_SAT_CONSOLE_DRILL", false,
    "Drill (BOTH peers): run -- a client types sd.cal, sauce.get and sv.hash at its SAT console; the host runs "
    "them on the terminal it keeps for that client and sends the lines back. rejoin -- the client leaves in the "
    "middle of an sd.calall (run with --rejoin-marker) and, back, is told its terminal is busy and gets the "
    "run's lines again. discard -- the only client leaves in the middle (run with --rejoin-marker and no observer): the "
    "session ends and the host checks, in the next one, that the discarded terminal's run stopped and it went at a "
    "GC. Lines tagged [SAT-DRILL].")
CFG_STRING(calib_drill, "calib_drill", "dev", "", "VOTVCOOP_CALIB_DRILL", false,
    "Drill (BOTH peers): the host holds four dishes at values no dish rests at. A client writes another value into "
    "its copy of the first, which the lane must put back within 3 s while the host's never takes it; writes it "
    "again and calls setPrec at once, whose average must be the host's; runs an uncalibrator's use on the second "
    "and a toolgun calibration tool's 2.5 on the third, which the host must perform and the client's copy hold, and "
    "the tool's NaN on the fourth, which the host must refuse and answer. "
    "run -- the host holds them once a client is in its world; join -- before any client connects. Lines tagged "
    "[CALIB-DRILL]; the client's DONE line ends it.")
CFG_STRING(server_drill, "server_drill", "dev", "", "VOTVCOOP_SERVER_DRILL", false,
    "Drill (BOTH peers): a server box's break is the host's and a client's repair of it runs there. The host breaks "
    "a box with no upgrades and stamps its repair type 123. run -- once a client is in its world: the client finds "
    "it broken with that type, its own breakServer on a healthy box is refused, it walks to the box and runs the "
    "repair widget's end(true), and the host's box ends fixed; join -- the break in the joiner's window, found after "
    "its world is ready. Lines tagged [SERVER-DRILL]; the client's DONE line ends it.")
CFG_STRING(server_upgrade_drill, "server_upgrade_drill", "dev", "", "VOTVCOOP_SERVER_UPGRADE_DRILL", false,
    "Drill (BOTH peers): a client installs an upgrade into a server box and takes one out. apply: the host "
    "applies both; refuse: the host refuses both, as if each had lost its race, and the client sees its install "
    "refunded and its take-out's upgrade go. Lines tagged [SRV-UPG-DRILL].")
CFG_STRING(power_decay_drill, "power_decay_drill", "dev", "", "VOTVCOOP_POWER_DECAY_DRILL", false,
    "Drill (BOTH peers): the generators' 30 s decay tick. watch: each peer says every tick's verdict, a client "
    "refusing its own and the host running its own, and the client's second tick prints the DONE line; red: a "
    "client runs its own tick, the negative control. Lines tagged [POWER-DECAY].")
CFG_STRING(desk_verb_drill, "desk_verb_drill", "dev", "", "VOTVCOOP_DESK_VERB_DRILL", false,
    "Drill (BOTH peers): the main desk's save family pressed by the client through its own input. run: the host "
    "arms a caught signal formed from a sky signal's object, downloaded and detected, and the client, walked to the "
    "desk, presses SAVE; then DELETE on a second one, the deck's send on the saved row, the deck's drive button "
    "both ways and the refiner's upload, then the refiner's start, its stop, a start the host completes and one at "
    "level 2 whose completion is the last level's. Both peers count the game's own entries into the functions those "
    "branches call; the host says it entered them, the client that it entered none, that its refiner never latched, "
    "that SAVE's and each completion's gloss and the last level's processed point reached its profile and the row "
    "its photo. join: the host decodes on its refiner and saves a row as the client joins; the client finds the row "
    "with its photo, and its restore of the decode refused, mirroring the host's. Lines tagged [DESK-VERB-DRILL]; "
    "'client DONE PASS|FAIL' ends it; ABANDONED is a step that could not run.")
CFG_STRING(desk_crossing_drill, "desk_crossing_drill", "dev", "", "VOTVCOOP_DESK_CROSSING_DRILL", false,
    "Drill (BOTH peers): the main desk's detection needle crosses on the host alone. run: two legs, planeteater's "
    "caught signal (its object renderer spawns the signal object fullyProcessed lives on) and looker_behind's; the "
    "host holds its needle just below 1 for three of its play cycles, then lets its step cross. The client says its "
    "own step never crossed, that it ran its desk's path after the step once, and on looker_behind that its autoSave "
    "body never ran while the host's saved the one row; at the crossing canDL holds on both, and on planeteater "
    "fullyProcessed. join: the host crosses on planeteater before the client's world is ready; the client relays no "
    "catch of its own, reads the host's needle at 1, and finds canDL and fullyProcessed. Lines tagged "
    "[DESK-CROSSING-DRILL]; 'client DONE PASS|FAIL' ends it; ABANDONED is a step that could not run.")
CFG_STRING(drive_drill, "drive_drill", "dev", "", "VOTVCOOP_DRIVE_DRILL", false,
    "Drill (BOTH peers): the host gives a drive a row; a client's deviation on its copy must be put back at once, "
    "its eraser press must wipe the drive on the host and show here, a drive it spawns, takes out or throws must "
    "reach the host with its row, and its forged row on a drive it did not bring must be refused. "
    "run -- a drive the host spawns once a client is in its world; join -- a drive of the host's world, written "
    "while a joiner loads it; obs -- run's steps, begun once two clients stand in the world, the second (the rig's "
    "observer) only watching the drive lane. Lines tagged [DRIVE-DRILL]; the host's DONE line ends it.")
CFG_STRING(tower_drill, "tower_drill", "dev", "", "VOTVCOOP_TOWER_DRILL", false,
    "Drill (BOTH peers): the coordinate towers. run: once the client stands in its world, the host breaks the "
    "drill's tower (the one nearest the alpha base) on its own dice and opens its panel; the client presses a "
    "puzzle button and pulls a blown fuse from where it stands, both refused out of reach and the pulled fuse "
    "reaped; then the host's player pulls a fuse, inserts one, presses a button, pulls the lever on the unsolved "
    "puzzle, solves it and pulls the lever again, and the client reads each change applied as the tower's own. "
    "join: the host breaks the tower as it hosts, and the joiner's tower reads the host's without a roll of its "
    "own. red, joinred: the controls, a client whose gates stand open and who applies no rows, as before the "
    "lane. Lines tagged [TOWER-DRILL]; the client's DONE line ends it.")
CFG_STRING(desk_ping_drill, "desk_ping_drill", "dev", "", "VOTVCOOP_DESK_PING_DRILL", false,
    "Drill (BOTH peers): a client's ping is rolled on the host. run: the client walks to the main desk, enters the "
    "coordinates screen, commits a triangle and presses its coord_ping key on the game window; legs: miss (a clear "
    "sky), catch (the host's fixture signal under the triangle), busy (a second verdict right behind the catch's). "
    "The client says its gatherSignal was entered and never ran, the host's failure line reached its log once, the "
    "catch is on its profile and desk, and the busy verdict came back refused; the host that it rolled two, caught "
    "the second without counting it as its own, relayed it as the client's, and refused the third. Lines tagged "
    "[DESK-PING-DRILL]; 'client DONE PASS|FAIL' ends it; ABANDONED is a step that could not run.")
CFG_STRING(download_drill, "download_drill", "dev", "", "VOTVCOOP_DOWNLOAD_DRILL", false,
    "Drill (BOTH peers): the host arms and resets its download five times; the client must replay each once with the "
    "host's values (formDownload's body once an arm, one reset line a reset, the reset clean) and have its own "
    "arm and reset refused. run -- once a client is in its world; join (with --rejoin and --rejoin-marker "
    "'[DOWNLOAD-DRILL] client LIFE 1 DONE') -- an arm while the client's first life is in, then in the rejoiner's "
    "window a reset, a second catch and its arm, which it must end armed on. Lines tagged [DOWNLOAD-DRILL]; the "
    "client's DONE line ends it.")
CFG_STRING(laptop_drill, "laptop_drill", "dev", "", "VOTVCOOP_LAPTOP_DRILL", false,
    "Drill (BOTH peers): the laptop's disc slot is the host's and its file edits land on the disc they were made on. "
    "run: the client inserts a disc and edits its files before the host answered the claim, ejects it, inserts a "
    "second; the host ejects that one ahead of the client's next edit, then a batch and a canonical of the ejected "
    "disc, each on its old generation, must land nowhere. join: the host inserts a disc and adds a buffer row while "
    "a joiner loads, and the joiner's laptop must hold both. Lines tagged [LAPTOP-DRILL]; the client's DONE line "
    "ends it.")
CFG_STRING(grid_drill, "grid_drill", "dev", "", "VOTVCOOP_GRID_DRILL", false,
    "Drill (BOTH peers): the power grid, arm by arm. run: the client presses the panel's light lever, the host "
    "breaks the drill's generator (the one nearest the panel), the client's Activate press from where it stands is "
    "refused beyond reach, and the host's own repair reaches the client. puzzle: the host's puzzle and inputs reach "
    "the client, whose own input from out of reach rolls back. puzzlesolve: the client, posed beside the generator, "
    "solves its puzzle and presses Activate, which the host takes; puzzleflood: from there it sends 40 inputs at "
    "once, which the host must take within its rate for inputs, its rows coalesced. upgrade: from that pose an "
    "install op with no "
    "insert behind it is refused, nothing refunded, the client's install is taken, and a refused one is refunded. "
    "join, lockout, lockjoin: the blackout at a join, the desk virus's lockout. red, puzzlered, upgradered: the "
    "controls (the rows applied raw, no puzzle written, installs judged unpaired). Lines tagged [GRID-DRILL]; the "
    "client's DONE line ends it.")
CFG_STRING(toggle_drill, "toggle_drill", "dev", "", "VOTVCOOP_TOGGLE_DRILL", false,
    "Drill (BOTH peers), by kind: garage, tap (a faucet or a sink), locker or lid. The device of that kind "
    "with the lowest key in its lane, toggled through its own verb by the client and toggled back by "
    "the host whenever it crossed; each peer logs every change of its state. Lines tagged "
    "[TOGGLE-DRILL]; the client's DONE line ends it. One way: oven (the client repairs the oven, the "
    "host reads it) and oven_host (the host repairs it once a client's world is ready); the host puts "
    "back an oven its save had repaired before any client connects, and the reader's DONE line ends it.")
CFG_STRING(door_drill_door, "door_drill_door", "dev", "", "VOTVCOOP_DOOR_DRILL_DOOR", false,
    "With door_drill: the object name of the one door the walker walks to (e.g. "
    "door_GEN_VARIABLE_door_C_CAT_45); empty walks the nearest one its navmesh reaches.")
CFG_FLAG(lookat_aim_drill, "lookat_aim_drill", "dev", false, "VOTVCOOP_LOOKAT_AIM_DRILL",
    "Drill (BOTH peers): walk to the nearest prop with the bot director, turn the camera onto it, "
    "and hold there without pressing anything, so the churn probe above has an aim to read. Says "
    "one readiness line once the game's own trace resolves the prop.")
CFG_FLAG(client_world_first, "client_world_first", "dev", false, "VOTVCOOP_CLIENT_WORLD_FIRST",
    "Rig (CLIENT): before it joins, the client boots a world of its own (a New Game: the rig starts every "
    "client fresh) and stands in it, then joins from inside it: the host's save downloaded and its world "
    "loaded once this one is left. A join that starts inside a world, which no menu-joining run exercises.")
CFG_FLAG(join_at_host, "join_at_host", "dev", false, "VOTVCOOP_JOIN_AT_HOST",
    "Drill (CLIENT): the join's first appearance keeps the transferred save's player transform, which "
    "is where the host stood, instead of this player's profile pose or the start point; a respawn "
    "still goes to the start point. For a drill that needs the client at the base: from the start "
    "point, a 770 m route, the director does not finish the walk on the rig. Off, the client appears "
    "at its profile's pose or at the start point.")
CFG_FLAG(rehost_rejoin, "rehost_rejoin", "dev", false, "VOTVCOOP_REHOST_REJOIN",
    "Drill (CLIENT): join the host again without restarting the game. At the main menu, with no "
    "session and no join in flight, a file named multivoid-rejoin.trigger beside the game exe is "
    "deleted and net.peer:net.port dialed through the direct connect, once per file. The rig's "
    "host-restart arm drops it when the relaunched host is up.")
CFG_FLAG(prop_birth_key_probe, "prop_birth_key_probe", "dev", false, "VOTVCOOP_PROP_BIRTH_KEY_PROBE",
    "Diagnostic probe (both roles): the place/birth seam -- how many drain ticks the save Key takes "
    "to restore after a spawn, and which of the seven drain exits each pending spawn leaves by; and "
    "every keyed prop it saw born that a script then destroys, with the session's state at the "
    "destroy, even with no session. The drain exits are the client's; the host places through a "
    "different path.")
CFG_FLAG(spawn_match_probe, "spawn_match_probe", "dev", false, nullptr,
    "Diagnostic probe: the OnSpawn fuzzy-match candidate set -- every same-class prop inside the "
    "30 cm radius with its slot, distance, own key and mirror binding, plus a watch that reports a "
    "destroy landing on an adopted actor. Walks the whole object array per fuzzy spawn while on.")
CFG_FLAG(eid_lifetime_trace, "eid_lifetime_trace", "dev", false, nullptr,
    "Diagnostic trace: entity-id lifetimes (mint/adopt/destroy).")
CFG_FLAG(pinecone_probe, "pinecone_probe", "dev", false, nullptr,
    "Diagnostic probe: pinecone prop behavior.")
CFG_FLAG(live_store_readout, "live_store_readout", "dev", false, nullptr,
    "Diagnostic readout: the live GObjStack container store.")
CFG_FLAG(hotbar_icon_probe, "hotbar_icon_probe", "dev", false, "VOTVCOOP_HOTBAR_ICON_PROBE",
    "Diagnostic readout: the quick-slot bar at world load -- the slot widget arrays, the "
    "prop-icon name table and texture cache, the carried count, and per slot which texture is bound "
    "to its `tex` parameter. Logs on change, so a settled bar is silent. The env twin exists because "
    "the question is whether the bar loses the race on ONE peer or both, which an ini-only row "
    "cannot arm in a single run.")
CFG_FLAG(heap_probe, "heap_probe", "dev", false, nullptr,
    "Diagnostic probe: process heap usage.")
CFG_FLAG(drone_probe, "drone_probe", "dev", false, nullptr,
    "Diagnostic probe: delivery drone state.")
CFG_FLAG(drone_probe_drive, "drone_probe_drive", "dev", false, nullptr,
    "Diagnostic probe: drive the delivery drone (test driver).")
CFG_FLAG(pile_dup_probe, "pile_dup_probe", "dev", false, nullptr,
    "Diagnostic probe: pile duplicate/identity mismatches.")
CFG_FLAG(perf_probe, "perf_probe", "dev", false, "VOTVCOOP_PERF_PROBE",
    "Performance probe: per-frame subsystem timing lines, the reflected-call rate and the ParamFrame allocation rate. The env twin exists so a launcher can arm BOTH peers for one run: an ini-only dev row has to be edited per install, which is how a perf number ends up measured on one peer and assumed on the other.")
CFG_FLAG(perf_probe_selftime, "perf_probe_selftime", "dev", false, nullptr,
    "Performance probe: self-time breakdown variant.")
CFG_FLAG(perf_probe_dispatch, "perf_probe_dispatch", "dev", false, "VOTVCOOP_PERF_PROBE_DISPATCH",
    "Performance probe: count every ProcessEvent dispatch (PE/s, game-thread share, our share). OFF by default -- gameplay dispatches ~170k/s across several threads, so this is a shared-atomic RMW on a contended cache line at that rate. Leave it off whenever the number being measured is what the mod itself costs; the 1 Hz fps line does not need it.")
CFG_INT(perf_probe_resdrop, "perf_probe_resdrop", "dev", 0, 0, 3600, nullptr,
    "Performance probe: after N 1 Hz samples, collapse render resolution (r.ScreenPercentage 25) to separate a GPU-bound frame from a CPU-bound one.")
CFG_INT(perf_probe_bypass, "perf_probe_bypass", "dev", 0, 0, 3600, nullptr,
    "Performance probe: after N samples, arm the transparent ProcessEvent bypass for 5 s (separates our dispatch-path cost from everything else about our presence).")
CFG_INT(perf_probe_statunit, "perf_probe_statunit", "dev", 0, 0, 3600, nullptr,
    "Performance probe: after N samples, issue `stat unit` + r.VSync 0 + t.MaxFPS 0. OFF by default because enabling any UE4 stat turns on FThreadStats collection engine-wide -- an instrument that changes the frame it measures. The delay exists so the samples before it are the uninstrumented baseline and the step across this line IS the instrument's own cost.")
CFG_FLAG(lightswitch_probe, "lightswitch_probe", "dev", false, "VOTVCOOP_LIGHTSWITCH_PROBE",
    "Diagnostic probe: light switch interactions + the light-group apply selftest. MUTATES the world (one-shot synthetic use()) -- never enable it on two peers at once. The env var exists so `mp.py lightgroup` can arm it: an ini-only dev row is unreachable to every launcher, which is how this selftest went unrun after a refactor.")
CFG_FLAG(lightgroup_census, "lightgroup_census", "dev", false, "VOTVCOOP_LIGHTGROUP_CENSUS",
    "Diagnostic census: per-key light GROUP state (isActive / active gate / buffIsActive) + switch A. Read-only; enable on BOTH peers and diff by key. It has an environment twin so a run can turn it OFF as its named variable: it dumps a line per group on a period, and any measurement of a per-frame or per-second effect has to be able to rule that out.")
CFG_FLAG(sleep_probe, "sleep_probe", "dev", false, nullptr,
    "Diagnostic probe: sleep/wake cycle.")
CFG_FLAG(script_gate_log, "script_gate_log", "dev", false, nullptr,
    "Diagnostic logging for the script-body gate's consumers (the kerfur form assembler's per-verb lines).")
CFG_FLAG(rng_roll_census, "rng_roll_census", "dev", false, nullptr,
    "Diagnostic census: RNG roll sites.")
CFG_FLAG(leak_probe, "leak_probe", "dev", false, nullptr,
    "Diagnostic probe: object/memory leak counters.")
CFG_FLAG(force_chippile_unclaim, "force_chippile_unclaim", "dev", false, nullptr,
    "Test driver: force chip-pile unclaim paths.")
CFG_FLAG(disable_completeness_floor, "disable_completeness_floor", "dev", false, nullptr,
    "Test driver: disable the per-class completeness floor (join sweep).")
CFG_FLAG(keypad_probe, "keypad_probe", "dev", false, nullptr,
    "Diagnostic probe: keypad state.")
CFG_FLAG(keypad_synth, "keypad_synth", "dev", false, nullptr,
    "Test driver: synthesize keypad presses (disruptive).")
CFG_FLAG(save_identity_map_log, "save_identity_map_log", "dev", false, nullptr,
    "Diagnostic logging for the save-identity map.")
CFG_FLAG(save_identity_bind, "save_identity_bind", "dev", false, nullptr,
    "Diagnostic: save-identity bind instrumentation.")
CFG_FLAG(force_kerfur_unmap, "force_kerfur_unmap", "dev", false, nullptr,
    "Test driver: force kerfur identity unmap paths.")
CFG_FLAG(force_save_churn, "force_save_churn", "dev", false, nullptr,
    "Test driver: force save churn (identity stress).")

// The two atlas drill knobs. The atlas watch ships two silent-degradation detectors, and a
// detector nobody has watched go red is decoration; each of these forces the exact condition
// one detector exists to catch, so the drill is a launch away instead of a source edit plus a
// rebuild, the difference between a drill that gets re-run after the next refactor and one
// that does not. Both default off; neither is reachable from any UI.
CFG_INT(atlas_texmax_drill, "atlas_texmax_drill", "dev", 0, 0, 8192,
    "VOTVCOOP_ATLAS_TEXMAX",
    "Override the font atlas size ceiling (0 = the shipped 2048). Set 256 to starve "
    "the packer and prove the pack-failure detector fires.")
// The admission drill, client side only, and that is the whole design: the host's gate has no
// knob, no dev branch and no env check anywhere, so the drill exercises the real refusal or
// it proves nothing; a knob on the host would make the drill's own pass conditional on a
// bypass existing in shipped code.
CFG_ENUM(auth_drill, "auth_drill", "dev", "off", "off|corrupt|silent|mismatch",
    "VOTVCOOP_AUTH_DRILL",
    "Sabotage THIS CLIENT's identity proof, to prove the host refuses it. corrupt = "
    "send a flipped signature; silent = open the exchange and never answer. Both must "
    "leave this peer unseated and with no save. mismatch = pretend we were sent to a "
    "DIFFERENT host than the one that answered, which THIS peer must refuse itself "
    "-- it is the only arm whose refusal is ours, not the host's. "
    "Drill only.")
CFG_FLAG(atlas_no_exclude_drill, "atlas_no_exclude_drill", "dev", false,
    "VOTVCOOP_ATLAS_NO_EXCLUDE",
    "Drop GlyphExcludeRanges from every font source, so the faces bake their whole "
    "cmaps. Proves the superset invariant fires. Breaks name folding -- drill only.")

// The native-UI probe, two knobs on purpose: the read half answers the questions the native
// server browser's design left unmeasured and touches nothing, while the write rung, the one
// that decides the screen's placement, writes into the live shipped widget switcher, and a
// write into the one hands-on-verified native inject does not ride the same opt-in as a log
// line.
CFG_FLAG(native_ui_probe, "native_ui_probe", "dev", false, "VOTVCOOP_NATIVE_UI_PROBE",
    "Diagnostic probe (READ-ONLY): UMG class/function resolution census, donor "
    "residency, the ui_menu switcher child map, and the count of frames presented "
    "while no world exists (decides whether the ImGui overlay substrate is retirable).")
CFG_FLAG(native_text_probe, "native_text_probe", "dev", false, "VOTVCOOP_NATIVE_TEXT_PROBE",
    "Diagnostic probe, and it WRITES: adds a UEditableTextBox to the live native "
    "browser panel, focuses it, POSTS characters into the game window, reads the "
    "field's own Text back, and removes it. Answers the one fact every native "
    "text-entry design forks on -- whether Slate routes keystrokes into a hand-wired, "
    "never-Initialize()d widget tree. Asserts on the TEXT, never on HasKeyboardFocus, "
    "which was measured lying about a field that was live at the time.")
CFG_FLAG(native_ui_probe_write, "native_ui_probe_write", "dev", false,
    "VOTVCOOP_NATIVE_UI_PROBE_WRITE",
    "RUNG 1 of native_ui_probe, and it WRITES: adds a throwaway UUserWidget to the "
    "live ui_menu widget switcher, shows it for ~2 s, then restores the previous "
    "active index and removes it. Main menu only, never with a session up. Requires "
    "native_ui_probe=1.")
// The native server browser is the default, permanently. This row is not a dev flag and not
// a trial: it is the user-facing fallback switch. On, the default, means the multiplayer
// button opens the native screen, drawn inside the game's own menu, obeying its palette and
// carrying its button sounds. Off is the deliberate way back to the overlay browser, which
// stays as the spare, a product decision about what the player wants. The known limit of the
// fallback: reaching it costs an ini edit plus a restart, so a player whose native browser
// breaks cannot fall back during the session that broke.
CFG_FLAG(browser_native, "browser_native", "ui", true, "VOTVCOOP_BROWSER_NATIVE",
    "Use the native in-game server browser (drawn inside VOTV's own menu). Turn this off "
    "to fall back to the older overlay browser; the change needs a restart.")
CFG_FLAG(browser_autoopen, "browser_autoopen", "dev", false, "VOTVCOOP_BROWSER_AUTOOPEN",
    "Show the native browser as soon as it is built, without a click -- an autonomous "
    "run cannot press the MULTIPLAYER button. Requires browser_native=1.")
CFG_FLAG(host_window_autoopen, "host_window_autoopen", "dev", false, "VOTVCOOP_HOST_WINDOW_AUTOOPEN",
    "Show the native HOST GAME window as soon as it is built, without a click -- the same "
    "reason browser_autoopen exists, for the screen the browser's Host button will open. "
    "Requires browser_native=1.")
CFG_FLAG(script_gate_drill, "script_gate_drill", "dev", false, "VOTVCOOP_RUN_SCRIPT_GATE_DRILL",
    "Drill (solo or host): the script-body gate refuses Blueprint verbs per call on one of two signal "
    "servers on every dispatch route, reads an out parameter through the frame, and watches two classes' "
    "ReceiveTick by class and name beside an unscoped control. MUTATES the two servers' break state, "
    "restored after.")
CFG_FLAG(class_lifetime_probe, "class_lifetime_probe", "dev", false, "VOTVCOOP_CLASS_LIFETIME_PROBE",
    "Probe: once per world, the menu's included, and every 30 s within one, every loaded Blueprint and "
    "native class judged against the identity it had when last seen (the same object, the same address "
    "holding a new one, a new address, or gone), with the functions its watched rows name judged the "
    "same way. Read-only but for the slot serials it assigns.")
CFG_FLAG(init_seam_probe, "init_seam_probe", "dev", false, "VOTVCOOP_INIT_SEAM_PROBE",
    "Probe: a script-gate watch on every Init body of a keyed prop, counted by the body's owning "
    "class, its caller, whether it runs inside another Init of the same object and whether the key "
    "is still unset; logged every 30 s and at each world change. Holds the gate; read-only.")
CFG_FLAG(selftest_break_permissions, "selftest_break_permissions", "dev", false, "VOTVCOOP_SELFTEST_BREAK_PERMISSIONS",
    "Red arm of the permissions selftest: its first case expects the opposite answer, so it must fail.")
CFG_FLAG(selftest_break_action_log, "selftest_break_action_log", "dev", false, "VOTVCOOP_SELFTEST_BREAK_ACTION_LOG",
    "Red arm of the commands selftest's action-log cases: the first acting case of each cases file expects "
    "no record, so it must fail.")
