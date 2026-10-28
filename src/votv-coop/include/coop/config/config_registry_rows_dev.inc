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
// The version-port tooling's rows, their own file.
#include "coop/config/config_registry_rows_dev_port.inc"
CFG_FLAG(desk_diag, "desk_diag", "dev", false, "VOTVCOOP_DESK_DIAG",
    "Diagnostic (BOTH peers): periodically log the signal desk's state, and the digests a day "
    "rollover or a world load rolls per machine (the dish hash codes, the coordinate towers, the "
    "servers' upgrades), to diff host against client. The env twin arms both peers for one run.")
CFG_INT(desk_diag_ms, "desk_diag_ms", "dev", 1000, 100, 60000, nullptr,
    "Interval in milliseconds for the desk_diag log.")
CFG_INT(vitals_keepalive_sec, "vitals_keepalive_sec", "dev", 0, 0, 86400, nullptr,
    "Reset player vitals every N seconds (0 = off; long unattended test runs).")
// The drills of a session's join, rejoin and leave: their own file, one concept, included as
// _dev_port.inc and _dev_boundary.inc are.
#include "coop/config/config_registry_rows_dev_join.inc"
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
CFG_FLAG(laptop_power_drill, "laptop_power_drill", "dev", false, "VOTVCOOP_LAPTOP_POWER_DRILL",
    "Autonomous drill: the stationary laptop's power, pressed on each peer; a replay must not come back as an edge.")
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
CFG_FLAG(rack_birth_drill, "rack_birth_drill", "dev", false, "VOTVCOOP_RACK_BIRTH_DRILL",
    "Drill (BOTH peers): once a client's world is ready the host spawns a drive rack holding four rows; the client "
    "must hold them and send the host no op. Lines tagged [RACK-BIRTH]; the client's DONE line ends it.")
CFG_FLAG(burn_revive_drill, "burn_revive_drill", "dev", false, "VOTVCOOP_BURN_REVIVE_DRILL",
    "Drill (the CLIENT): the player is set on fire, dies, and must be revived not burning. Lines tagged "
    "[BURN-REVIVE]; the client's DONE line ends it.")
CFG_FLAG(desk_snd_selftest, "desk_snd_selftest", "dev", false, nullptr,
    "Autonomous selftest: desk sound mirror.")
// The container rows (selftest, join drill and its red arms, opener probe, view drill): their own file, one concept,
// included as _dev_port.inc is above.
#include "coop/config/config_registry_rows_dev_container.inc"
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
CFG_FLAG(puppet_reg_probe, "puppet_reg_probe", "dev", false, "VOTVCOOP_PUPPET_REG_PROBE",
    "Probe (BOTH peers): each remote player's puppet judged at its begin-play and at its end of play -- "
    "registered in the player registry or not -- one [PI-0] line per puppet life. Read-only.")
CFG_FLAG(pi0_selftest, "pi0_selftest", "dev", false, "VOTVCOOP_PI0_SELFTEST",
    "Autonomous selftest (HOST): a Player Element whose puppet was cleared keeps its id when the retry's "
    "puppet is registered -- one [PI-0-SELFTEST] line, on a spare peer slot, the registry left as found.")
CFG_FLAG(hurt_flash_gc_probe, "hurt_flash_gc_probe", "dev", false, "VOTVCOOP_HURT_FLASH_GC_PROBE",
    "Probe (the peer that draws a puppet): a garbage collection is forced as each hurt flash starts, and "
    "the class of slot 0's material on both body meshes is logged before the flash and after its restore "
    "-- one [HFG] line each -- so a skin material collected during the flash shows as a changed class.")
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
CFG_ENUM(remover_drill, "remover_drill", "dev", "off", "off|granted|withheld", "VOTVCOOP_REMOVER_DRILL",
    "Drill (BOTH peers): the tool gun's remover pressed by a client at props the host spawned about its puppet, "
    "each peer judging its own copies. granted: one prop, six presses at once on another, every distinct ring prop in "
    "sight one after another, then a creature the remover cannot remove -- the props must go on both peers and the "
    "creature stay. withheld: one prop, which the host's permissions refuse to this client and which must stay on "
    "both peers. The client ends with a chat line the host judges at.")
CFG_ENUM(chat_drill, "chat_drill", "dev", "off", "off|history|seed|i18n|span", "VOTVCOOP_CHAT_DRILL",
    "Developer: chat checks judged in memory -- history (the host), seed (the second joiner), i18n "
    "(every peer), span (every peer: a mid-line nick's span through the feed); each prints "
    "[CHAT-DRILL] PASS, FAIL or ABORT.")
CFG_ENUM(l10n_drill, "l10n_drill", "dev", "off", "off|translated|english|atlas", "VOTVCOOP_L10N_DRILL",
    "Developer: the interface-language drill on each peer -- opens two F1 panes, waits for the join line, "
    "then expects them translated or English; atlas floods the chat with ideographs under the Chinese pack "
    "and measures the font atlas; prints [L10N-DRILL] DONE or FAIL.")
CFG_FLAG(chat_span_prefix, "chat_span_prefix", "dev", false, "VOTVCOOP_CHAT_SPAN_PREFIX",
    "Developer: keep a peer action's nick as a prefix wherever its sentence put it (the chat drill's span red).")
CFG_FLAG(chat_drill_bad_nick, "chat_drill_bad_nick", "dev", false, "VOTVCOOP_CHAT_DRILL_BAD_NICK",
    "Developer: the chat drill announces a wrong nickname (the i18n red).")
CFG_FLAG(chat_no_retain, "chat_no_retain", "dev", false, "VOTVCOOP_CHAT_NO_RETAIN",
    "Developer: never keep a chat line in history (the chat drill's history red).")
CFG_FLAG(chat_seed_suppress, "chat_seed_suppress", "dev", false, "VOTVCOOP_CHAT_SEED_SUPPRESS",
    "Developer: send a joiner no chat history (the seed red).")
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
    "nearest keyed physics prop to itself, walks out and back with the bot director so the "
    "constraint drags it, then drops the hook; the other peer takes the prop that hook bites, "
    "and both log its position on one clock. It MUTATES the world, so arm it from the "
    "environment for one run, on BOTH peers.")
CFG_ENUM(hookdrag_role, "hookdrag_role", "dev", "host", "host|client|both", "VOTVCOOP_HOOKDRAG_ROLE",
    "Which peer plants the hook in the hookdrag selftest: the host (the driven-prop channel alone), "
    "the client (the host ties the client's hook on its mirror), or both (each into the nearest "
    "prop to itself: two host constraints on one body when they stand together). Set the same "
    "value on BOTH peers.")
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
CFG_FLAG(puppet_head_probe, "puppet_head_probe", "dev", false, nullptr,
    "Diagnostic probe: puppet head/aim pose.")
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
CFG_ENUM(midnight_drill, "midnight_drill", "dev", "off", "off|awake|asleep|cheat|mode5|malformed|joinwindow",
    "VOTVCOOP_MIDNIGHT_DRILL",
    "Drill (BOTH peers, with rollover_watch), once a client's join is over. awake and asleep: the "
    "host sets its clock just short of midnight; awake: both peers stay up; asleep: the host goes to "
    "bed and the client follows once it sees the host asleep, so the midnight falls inside the host's "
    "own fast-forward (each peer sleeps on its own). cheat: the host leaves its "
    "clock alone and the client writes a day onto its own, three times, as the cheat menu's day "
    "button does. mode5: each peer spawns game mode 5's master, which resets the day every second, "
    "and the client says whether its own reset kept running. malformed: the host puts three clock "
    "samples whose day is not a number, and the client's log says where each was refused. joinwindow: "
    "the host sets its clock just short of midnight once it has taken a joiner's world, so the midnight "
    "falls inside the join, and both peers print their dishes' hash digest; equal is a pass. Run it on a "
    "fresh host world; every refused step is logged and ends the arm as invalid.")
CFG_ENUM(sleep_drill, "sleep_drill", "dev", "off", "off|host_asleep|client_rest|beside", "VOTVCOOP_SLEEP_DRILL",
    "Drill (BOTH peers), once a client's join is over: what a host's sleep and a client's rest do. "
    "host_asleep: the host sets its need to 80 and sleeps natively and says the world's dilation it "
    "reads (20); the client reads its own world's dilation (1) and its clock for 10 s, and the clock "
    "must run at 0.9 of the native rate. client_rest: the host sleeps and prints its real seconds; "
    "when it wakes the client reads its food for 10 s, sleeps from a need of 80 in the nearest bed "
    "and prints its real seconds, its world's dilation, and its food and need at both ends. The host "
    "holds its nightmare chance at 0 for the arm. beside: the host sleeps and is held asleep (its need is "
    "written to 50 every tick) until the hook drag selftest has ended on it, then wakes; run it with "
    "hookdrag_selftest=1 and hookdrag_role=client on both peers. The client prints its armed line only. "
    "Every phase ends on a verdict line "
    "([SLEEP-DRILL] <arm> <role> PASS|FAIL), and a phase that does not come is a FAIL after a bound.")
CFG_ENUM(badsun_drill, "badsun_drill", "dev", "off", "off|clock|midnight|spawn|cheat|join|end|absent_stale", "VOTVCOOP_BADSUN_DRILL",
    "Drill (BOTH peers), the Bad Sun: does a peer's own loaded copy go "
    "active by its own clock. clock, join and end need the save that holds one saved Bad Sun; the others a fresh host world. clock: the host parks the clock at 05:00 at hosting and sets 06:30 once a "
    "client's join is over; each peer reads its count of live copies (it must be 1), its copy's widget "
    "and its place in the active events' senders, and its own clock every second, from before 07:00; "
    "the copy must stay inactive until the clock reads 07:00 and be active by both tests within 15 s "
    "after it. Each peer ends on a verdict line ([BADSUN-DRILL] clock <role> PASS|FAIL|INCONCLUSIVE; "
    "INCONCLUSIVE: the save's copy was active at hosting, or the peer did not see its clock cross 07:00). "
    "midnight, spawn, cheat, join, end and absent_stale: whether the host's existence of a Bad Sun reaches "
    "each peer's own copies through the lane (the day edge's in-graph spawn, the host's call, a client's own refused "
    "call, a joiner's seed, the end by the host's clock, a stale copy in a joiner's blob); each arm's set-up "
    "and verdicts are in src/coop/dev/badsun_drill_shared.h, and each ends on its verdict line.")
CFG_ENUM(event_relay_drill, "event_relay_drill", "dev", "off", "off|on", "VOTVCOOP_EVENT_RELAY_DRILL",
    "Drill (BOTH peers, one run, one rejoin), the host's creature events in a client's event count: the host "
    "spawns an inert krampus 15 m above itself once the client's join is over; the client reads its mirror's "
    "copies in the gamemode's event senders (it must be 1, the relay's one), its count and the game's getEvent, "
    "tells the host by chat, and rejoins and reads the same; the host then destroys the krampus and the client's "
    "count must fall by one while the host's returns to where it began. Each step ends on a [EVR] PASS or "
    "[EVR] FAIL line (src/coop/dev/event_relay_drill.cpp); a step that does not come is a FAIL after 60 s.")
CFG_ENUM(creature_hit_drill, "creature_hit_drill", "dev", "off", "off|zombie|fossilhound", "VOTVCOOP_CREATURE_HIT_DRILL",
    "Drill (BOTH peers, one run), a parked creature's hit and a client's hit on it: the host spawns the creature "
    "300 cm in front of the client's puppet once the client's join is over; its hits on the puppet must reach the "
    "client three times and none may come from the mirror's own copy; then the client, with an empty hand and "
    "then with an axe, calls the mirror's addDamage, and the host must refuse the first and judge the rest until "
    "its creature dies and the mirror is destroyed. The fossilhound arm adds the mirror's own per-viewer event. "
    "Each step ends on a [CHD] PASS or [CHD] FAIL line (src/coop/dev/creature_hit_drill.cpp); a step that does "
    "not come is a FAIL after 60 s.")
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
CFG_FLAG(log_drill, "log_drill", "dev", false, "VOTVCOOP_LOG_DRILL",
    "Drill (one install, two processes): at boot each process says whose log it writes; one that found the usual "
    "log held passes when that log names another process. Run with mp.py twolog.")
CFG_STRING(throw_drill, "throw_drill", "dev", "", "VOTVCOOP_THROW_DRILL", false,
    "Drill (BOTH peers): a container a client throws keeps its contents on the host. run -- the host fills a backpack "
    "before the client's puppet, the client throws it, the host's copy must match; red and gone -- with "
    "container_birth_skip_slice=1 on the client, the copy stays empty (red) or the host kicks the thrower (gone). "
    "The host's DONE line ends it.")
CFG_FLAG(container_birth_skip_slice, "container_birth_skip_slice", "dev", false, "VOTVCOOP_CONTAINER_BIRTH_SKIP_SLICE",
    "Developer: a container this client throws sends no contents slice (the throw drill's red and gone).")
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
CFG_ENUM(prop_syncer_drill, "prop_syncer_drill", "dev", "off", "off|kick|forge", "VOTVCOOP_PROP_SYNCER_DRILL",
    "Drill (host and BOTH clients, the same value; three peers): a client syncs a prop its own interaction moves. "
    "kick: the client in slot 1 walks to a keyed prop with the director, claims it, kicks it with the game's own "
    "physics and holds the claim for a second; the host and the client in slot 2 must each take at least three rows "
    "of its generation, see the prop travel, and rest within 5 cm / 3 deg of the client's pose. forge: the client in "
    "slot 2 also publishes rows for the kicked prop at its own place; the host must count them as from a non-syncer, "
    "admit none and still land on the client's pose. Lines tagged [PROP-SYNC]; the verdict is DONE PASS or FAIL:.")
CFG_ENUM(prop_syncer_red, "prop_syncer_red", "dev", "off", "off|refuse|no_relay|not_syncer", "VOTVCOOP_PROP_SYNCER_RED",
    "Developer red arm: never set. refuse (HOST): every claim is refused as paced, so the kick arm's client must fail. "
    "no_relay (HOST): an admitted client row is applied to the host's copy and never re-published, so the kick arm's "
    "observer must fail. not_syncer (HOST): the sender test of admission is skipped, so the forge arm's host must fail.")
CFG_INT(join_gate_red, "join_gate_red", "dev", 0, 0, 1, "VOTVCOOP_JOIN_GATE_RED",
    "Developer red arm: never set (HOST). 1 bypasses the join gate: a joining slot's sends are not held behind its "
    "snapshot, so the receive parks catch the seeds that precede their props, and the join drills' park counts go "
    "nonzero.")
CFG_INT(bind_wait_red, "bind_wait_red", "dev", 0, 0, 1, "VOTVCOOP_BIND_WAIT_RED",
    "Developer red arm: never set. 1 makes the bind wait refuse every hold, so a message for a prop whose bind is "
    "deferred is dropped and counted as never bound, so the drills that count what the wait replayed must fail.")
CFG_FLAG(window_proc_red, "window_proc_red", "dev", false, "VOTVCOOP_WINDOW_PROC_RED",
    "Developer red arm: never set. 1 makes game_window_proc never subclass the game window, so no key reaches a "
    "native text field and the server browser selftest's INPUT TYPE phase must FAIL with \"the characters did not "
    "arrive\".")
// A drill seam, never on by default: the late-join drill sets it on the JOINER so that every seed landing before
// its spawn is held and replays at the bind, and every drill line that uses it names it. Not with
// bind_wait_selftest: its first check expects an unmarked eid to be refused.
CFG_FLAG(bind_wait_mark_unknown, "bind_wait_mark_unknown", "dev", false, "VOTVCOOP_BIND_WAIT_MARK_UNKNOWN",
    "Drill (the JOINER): the bind wait marks an eid this peer has no registry row for as deferred when a message "
    "for it arrives, so a seed that lands before its spawn is held and replays at the spawn's bind instead of "
    "being dropped as never bound. Pair with join_gate_red=1 on the host. Not with bind_wait_selftest, whose first check fails under it.")
CFG_INT(bind_wait_selftest, "bind_wait_selftest", "dev", 0, 0, 1, "VOTVCOOP_BIND_WAIT_SELFTEST",
    "Selftest (a CLIENT): once its world is announced, the bind wait's rules are checked on synthetic eids: a hold for "
    "an eid not deferred is refused, holds replay in order with the coalesced one in its place, a replay that holds "
    "again does not corrupt the pump, a given-up eid and a slot that left take their holds with them (also during a "
    "pump), a row awaiting its re-bind holds, the 65th hold of one eid is refused and counted, a module's Drop voids "
    "its stored, due and in-pass holds, and the per-kind tallies tell a coalesced hold from a distinct one and balance, a client slot has its own share of the pool. Its last check Clears the wait, ending the session's real marks. Prints [BIND-WAIT-SELFTEST] PASS or FAIL. With bind_wait_red=1 it prints RED ARM and no PASS.")
CFG_ENUM(prop_syncer_load_lane, "prop_syncer_load_lane", "dev", "transfer", "transfer|default", "VOTVCOOP_PROP_SYNCER_LOAD_LANE",
    "Drill knob (HOST): the lane the load arm's filler rides. transfer is the shipped lane of a payload that can "
    "exceed the blob ceiling; default puts the filler on the ordered lane the syncer's own messages ride, which is "
    "the red: a 4 MiB filler then stands ahead of the claim's answer and the grant misses its bound.")
CFG_INT(prop_syncer_load_kb, "prop_syncer_load_kb", "dev", 0, 0, 16384, "VOTVCOOP_PROP_SYNCER_LOAD_KB",
    "Drill (host and the kicking client, the same value; arm kick or forge): the filler the host queues on the transfer "
    "lane to the kicking client right before it says go, in KiB (0 = none). The client waits for the filler's first "
    "message before it claims, so its claim stands behind the transfer; its DONE line carries the grant latency and "
    "the filler counts.")
CFG_FLAG(drone_call_drill, "drone_call_drill", "dev", false, "VOTVCOOP_DRONE_CALL_DRILL",
    "Drill (BOTH peers): the client walks to the garage's drone console, opens its lid if it is shut, presses "
    "the leave-timer face twice (the click sounds on its copy and the host's stream flips the timer, then flips "
    "it back), then aims at the keyboard and presses it through the player's own E dispatch; each press goes to "
    "the host, which presses its console for the client. A sack the first press brought is followed by a "
    "second press, and the client's DONE line says whether the drone flew on its copy; the host logs its "
    "drone's changes. "
    "Lines tagged [DRONE-CALL-DRILL].")
CFG_INT(dronesack_drill, "dronesack_drill", "dev", 0, 0, 4, "VOTVCOOP_DRONESACK_DRILL",
    "Drill (BOTH peers, the same value): the host puts a drone sack on the floor in front of itself and the client "
    "says its mirror of it is bound. 1 -- the host drops a loot crate onto the sack; for 15 s each peer logs the "
    "crate's local destroy (and the caller's class), the client whether its destroy seam sent a PropDestroy, the "
    "host whether one reached it, and the sack's record counts. 2 -- the host spawns six loose props round the "
    "sack and the client walks to each with the director, grabs it, carries it over the sack and lets go; each "
    "peer logs the destroys it sees and the host then calls the delivery drone and logs when its sale clears the "
    "drone's container. 3 -- the host itself drops six cups and one fresh crate onto the sack, one at a time, then "
    "calls the drone; both peers take the container census around the sale and log the crate's "
    "inventory index and which containers hold it afterwards. It judges nothing. Lines tagged [SACK-DRILL] and "
    "[SACK-CENSUS]. 4 -- arm 3, then the client reads its sack mirror 1, 5, 15 and 30 s after the sale; if the "
    "mirror still holds records at 5 s it takes record 0 out through the container's getObject, the call a "
    "player's take makes. The host says what the container lane made of that.")
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
CFG_STRING(camera_drill, "camera_drill", "dev", "", "VOTVCOOP_CAMERA_DRILL", false,
    "Drill (BOTH peers): does a picked-up camera stick to its wall again, and by whose hand. grab -- the host hangs "
    "a camera on a wall a walker reaches (before any client joins); the client finds it stuck, walks to it, grabs "
    "it, holds 3 s of world time, lets go and watches 2.5 s more; the prop-stick lines of both peers carry the "
    "cause. grab_near -- the same, the walk ending as close to the wall as the director allows. host_grab_near -- "
    "the host does the near grab on its own camera (the control; no client needed). Lines tagged [CAMERA-DRILL]; "
    "the grabber's DONE line ends it. The state arms, each ended by the client's \"[CAMERA-DRILL] <arm> PASS|FAIL\" "
    "line: host-toggle -- the host switches a camera on, the client reads it on; client-toggle -- the client's press "
    "is refused here and performed by the host; light -- the same for the laptop's camera-light button, after a walk "
    "to the laptop; host-event -- the host fires the gray event beside a switched-on camera, the client reads it on "
    "and then off; join -- the host switches both bits on before any client joins, every session reads both.")
CFG_STRING(sleepingbag_drill, "sleepingbag_drill", "dev", "", "VOTVCOOP_SLEEPINGBAG_DRILL", false,
    "Drill (BOTH peers): a sleeping bag unrolls and folds once on each peer, one for one. run -- once a client is "
    "in its world the host drops a packed bag on a floor a route reaches; the client walks to it, unrolls it, "
    "waits 5 s of world time to see its deployed form stand, and folds it; then the host unrolls and folds it "
    "itself and the client reads each change; last, the host drops a bunk mattress on another empty floor and the "
    "client takes it off. Each peer judges its own copy: the old form gone, exactly one new form with an element id. "
    "Lines tagged [BAG-DRILL]; the client's DONE line ends it.")
CFG_STRING(notebook_drill, "notebook_drill", "dev", "", "VOTVCOOP_NOTEBOOK_DRILL", false,
    "Drill (EVERY peer): a notebook's page is written at one peer's editor and read on the others. run -- once a "
    "client is in its world the host drops a notebook on a floor a route reaches and writes a seed page; the client "
    "walks to it, opens it with the notebook's own action and holds its claim; the host presses the open on its own "
    "copy, and the claim sends its editor out again; the client types a page and leaves the editor open, and the "
    "host reads it (the publish at the editor's commit) and checks that its rule refuses a slot that is not the holder; the client then "
    "writes a last page and closes with the editor's own quit, and the host's copy must read it, taken from the slot "
    "that still held the claim. A second client (the rig's "
    "--observer) reads the page too. rejoin -- the same, and the client's first life ends on \"client LIFE DONE\"; its "
    "second life reads the page from the snapshot. red -- the editor is closed with the game's discard (skipEdit), "
    "so no page is carried and the host's read must fail. Lines tagged [NOTE-DRILL]; each peer's DONE line ends it.")
CFG_STRING(laptop_tab_drill, "laptop_tab_drill", "dev", "", "VOTVCOOP_LAPTOP_TAB_DRILL", false,
    "Drill (BOTH peers): everyone sees the tab the base computer's user is on. The user walks to the PC with the "
    "director, enters it and switches tabs; the other peer judges the LAST tab on arrival of a state carrying it. tab "
    "-- the client holds, the host reads tab 10; tab-host -- the host holds, the client reads; tab-handover -- the "
    "client holds and leaves, the host enters on the shared tab and switches, the client reads it; tab-norc -- a "
    "viewer reads the cameras tab with its createRC run 0 times; tab-subgame -- the sub-game button's tab reaches the "
    "host; tab-range -- a holding client sends a raw index past the switcher's last, the host drops and counts it as "
    "a bad index; tab-forged -- a client holding nothing sends a report, the host drops and counts it as not the "
    "holder's; tab-slot -- the host hands the client a state stamped with another sender's slot, which it drops and "
    "counts as a bad slot; tab-join -- the host switches to tab 4 before any client connects, and every joiner reads "
    "it; tab-relay (needs --observer) -- the first client holds, the second reads, and the first is sent no state. "
    "Lines tagged [LAPTOP-TAB-DRILL]; the judging peer's PASS or FAIL line ends the arm.")
CFG_STRING(power_decay_drill, "power_decay_drill", "dev", "", "VOTVCOOP_POWER_DECAY_DRILL", false,
    "Drill (BOTH peers): the generators' 30 s decay tick. watch: each peer says every tick's verdict, a client "
    "refusing its own and the host running its own, and the client's second tick prints the DONE line; red: a "
    "client runs its own tick, the negative control. Lines tagged [POWER-DECAY].")
// The signal workstation's drills: their own file, one concept, included as _dev_swinger.inc is below.
#include "coop/config/config_registry_rows_dev_desk.inc"
CFG_STRING(toggle_drill, "toggle_drill", "dev", "", "VOTVCOOP_TOGGLE_DRILL", false,
    "Drill (BOTH peers), by kind: garage, tap (a faucet or a sink), locker or lid. The device of that kind "
    "with the lowest key in its lane, toggled through its own verb by the client and toggled back by "
    "the host whenever it crossed; each peer logs every change of its state. Lines tagged "
    "[TOGGLE-DRILL]; the client's DONE line ends it. One way: oven (the client repairs the oven, the "
    "host reads it) and oven_host (the host repairs it once a client's world is ready); the host puts "
    "back an oven its save had repaired before any client connects, and the reader's DONE line ends it.")
// The swinger drill's row: its own file, one concept, included as _dev_nb.inc is below.
#include "coop/config/config_registry_rows_dev_swinger.inc"
CFG_STRING(door_drill_door, "door_drill_door", "dev", "", "VOTVCOOP_DOOR_DRILL_DOOR", false,
    "With door_drill: the object name of the one door the walker walks to (e.g. "
    "door_GEN_VARIABLE_door_C_CAT_45); empty walks the nearest one its navmesh reaches.")
CFG_FLAG(lookat_aim_drill, "lookat_aim_drill", "dev", false, "VOTVCOOP_LOOKAT_AIM_DRILL",
    "Drill (BOTH peers): walk to the nearest prop with the bot director, turn the camera onto it, "
    "and hold there without pressing anything, so the churn probe above has an aim to read. Says "
    "one readiness line once the game's own trace resolves the prop.")
// The diagnostic probes and censuses of the entity and prop seams: their own file, one concept, included as
// _dev_port.inc is above.
#include "coop/config/config_registry_rows_dev_probes.inc"
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
    "Override the font atlas size ceiling (0 = the shipped one: 2048, 4096 under a script face). Set 256 to starve "
    "the packer and prove the pack-failure detector fires.")
// The receive boundary's drills (admission forgery, the hostile-peer drill): their own file, one
// concept, included as _dev_port.inc is above.
#include "coop/config/config_registry_rows_dev_boundary.inc"
CFG_FLAG(l10n_selftest_red, "l10n_selftest_red", "dev", false, "VOTVCOOP_L10N_SELFTEST_RED",
    "The l10n selftest's red arm: one expected plural form is made wrong, so the selftest says FAIL "
    "and the rig's verdict reads it. Changes nothing else.")
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
CFG_FLAG(key_parity_probe, "key_parity_probe", "dev", false, "VOTVCOOP_KEY_PARITY_PROBE",
    "Probe: a shadow prop key index fed only by script-gate POST watches on init, setKey, getKey and "
    "loadData, and the key index the mod keeps, each compared every 2 s against a walk of the live keyed "
    "props; logs the props each lacks or holds under another key, with examples per world. Holds the "
    "gate; read-only but for the slot serials it assigns.")
CFG_FLAG(key_index_forget_one, "key_index_forget_one", "dev", false, "VOTVCOOP_KEY_INDEX_FORGET_ONE",
    "Red arm of the prop key index's parity pass: once a session, after the seed, one filed prop of the "
    "current world is dropped from the index. The pass must name it as a filing the feeds missed and file "
    "it again within a full pass (about 20 s).")
// The permissions and action-log selftests' red arms: their own file, one concept, included as
// _dev_port.inc is above.
#include "coop/config/config_registry_rows_dev_selftest.inc"
// The explosion lane's drill, birth line and selftest red arms: their own file, one concept, included as
// _dev_port.inc is above.
#include "coop/config/config_registry_rows_dev_explosion.inc"
// The Geom Octa lane's drill and red arm: their own file, one concept, included as _dev_port.inc is above.
#include "coop/config/config_registry_rows_dev_octa.inc"
// The fire lane's drill: its own file, one concept, included as _dev_port.inc is above.
#include "coop/config/config_registry_rows_dev_fire.inc"
// The cord lane's drill: its own file, one concept, included as _dev_port.inc is above.
#include "coop/config/config_registry_rows_dev_cord.inc"
// The coffee lane's drill and red arm: their own file, one concept, included as _dev_port.inc is above.
#include "coop/config/config_registry_rows_dev_coffee.inc"
// The pile deferral drill's row: its own file, one concept, included as _dev_port.inc is above.
#include "coop/config/config_registry_rows_dev_pile.inc"
// The ATV lane's rows (its probe, corrector arm, spawn drill, health drill): their own file, as _dev_port.inc is above.
#include "coop/config/config_registry_rows_dev_atv.inc"
// The server boxes' drills (the repair and its pay, the upgrade install): their own file, one concept, included
// as _dev_port.inc is above.
#include "coop/config/config_registry_rows_dev_server.inc"
// The puppet-identity arc's selftests and drill (PI-1, PI-2, PI-3): their own file, one concept, included as
// _dev_port.inc is above.
#include "coop/config/config_registry_rows_dev_pi.inc"
// The NPC-spawner arc's native-spawn probe (NB): the same shape, the last rows of the list.
#include "coop/config/config_registry_rows_dev_nb.inc"
// The Vulkan overlay arc's probes: their own file, one concept, included as _dev_nb.inc is above.
#include "coop/config/config_registry_rows_dev_vulkan.inc"
