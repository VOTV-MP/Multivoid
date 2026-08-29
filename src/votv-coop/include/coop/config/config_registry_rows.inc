// coop/config/config_registry_rows.inc -- THE one registry row list (ini rework
// arc 3, T2-migrate; arc 4 adds the desc/gatedBy columns for the T8 catalog).
// X-macro'd: config_registry.h expands it into the typed handle DECLARATIONS
// (rows::<ident>), config_registry.cpp expands it three times (index enum /
// Row table / handle definitions) -- ONE list, every artifact, no
// transcription (the drift class T2 exists to kill).
//
// Entry forms (ident = the C++ handle name; key = canonical ini spelling;
// desc = the T8 catalog's human text -- SEMANTICS ONLY: allowed tokens,
// ranges and the env twin are generator-emitted from the columns, never
// written into desc):
//   CFG_FLAG    (ident, key, section, defB, envVar, desc)
//   CFG_INT     (ident, key, section, defI, lo, hi, envVar, desc)
//   CFG_FLOAT   (ident, key, section, defF, lo, hi, envVar, desc)
//   CFG_ENUM    (ident, key, section, defS, tokens, envVar, desc) // defS "" =
//               // unset sentinel, ONLY net.role / net.ice (ValidateRows)
//   CFG_STRING  (ident, key, section, defS, envVar, seeded, desc)
//   CFG_STRING_GATED(ident, key, section, defS, envVar, seeded, gatedBy, desc)
//               // gatedBy = the Flag row key that gates the INI read
//               // (ValidateRows: must exist + be a Flag row)
//   CFG_IDENTITY(ident, key, section, desc)                // minted, no def
//   CFG_FONTROLE(ident, key, roleSuffix, defFamilyIndex, desc)
//
// DEFAULTS carry the arc-3 migration (BEFORE harvest 2026-07-25: 100 keys,
// 108 sites, ZERO same-key conflicts) or ALIAS the one owning constant
// (kDefaultPort / kOfficialMasterUrl / kOfficialSignalingUrl / kMyNameDefault).

// ---- [net] ------------------------------------------------------------------
CFG_STRING(net_nick, "net.nick", "net", kMyNameDefault, "VOTVCOOP_NET_NICK", true,
    "Your display name, shown to other players (nameplates, chat, the scoreboard).")
CFG_ENUM(net_role, "net.role", "net", "", "host|client", "VOTVCOOP_NET_ROLE",
    "Auto-start role for the LAN test framework. Leave unset for normal play -- the in-game "
    "Multiplayer menu handles hosting and joining.")
CFG_STRING(net_peer, "net.peer", "net", "127.0.0.1", "VOTVCOOP_NET_PEER", false,
    "Test framework: the host address a client connects to at auto-start (lan topology).")
CFG_INT(net_port, "net.port", "net", ::coop::net::kDefaultPort, 1, 65535, "VOTVCOOP_NET_PORT",
    "UDP port the host listens on (lan topology).")
CFG_ENUM(net_topology, "net.topology", "net", "lan", "lan|p2p", "VOTVCOOP_NET_TOPOLOGY",
    "Transport shape: lan = direct UDP, p2p = internet session via the signaling server "
    "(NAT traversal, no port forwarding needed).")
CFG_INT(net_sendbuf_kb, "net.sendbuf_kb", "net", 0, 0, 262144, "VOTVCOOP_NET_SENDBUF_KB",
    "Per-connection network send buffer, KB. 0 = the mod's default. Testing knob -- lowering "
    "it forces send-buffer backpressure (the delivery-queue drill).")
CFG_INT(net_sendrate_kbs, "net.sendrate_kbs", "net", 0, 0, 1048576, "VOTVCOOP_NET_SENDRATE_KBS",
    "Fixed network send rate, KB/s (sets the transport's min+max rate clamp). 0 = the mod's "
    "default. Testing knob -- lowering it simulates a slow link end-to-end.")
CFG_INT(net_fakelink_kbs, "net.fakelink_kbs", "net", 0, 0, 1048576, "VOTVCOOP_NET_FAKELINK_KBS",
    "Simulated OUTBOUND link capacity, KB/s (GNS FakeRateLimit_Send: a process-global policer "
    "that silently DROPS packets beyond the budget -- worst-case thin-uplink physics). 0 = off. "
    "Testing knob for the overdrive drill; pair with net.sendrate_kbs above this value.")
CFG_STRING_GATED(net_master, "net.master", "net", ::coop::net::kOfficialMasterUrl,
    "VOTVCOOP_MASTER_URL", false, "net.master.custom",
    "Master (lobby list) server, host:port. The special value DEFAULT also means the official "
    "server.")
CFG_FLAG(net_master_custom, "net.master.custom", "net", false, nullptr,
    "Set to 1 to use your own master/signaling servers from net.master / net.signaling instead "
    "of the official ones (run-your-own-master).")
CFG_STRING_GATED(net_signaling, "net.signaling", "net", ::coop::net::kOfficialSignalingUrl,
    "VOTVCOOP_NET_SIGNALING", false, "net.master.custom",
    "Signaling (rendezvous) server for internet sessions.")
CFG_STRING(net_signaling_token, "net.signaling_token", "net", "", "VOTVCOOP_NET_SIGNALING_TOKEN", false,
    "Access token for a self-hosted signaling server. The official flow mints per-session "
    "tokens automatically -- leave empty.")
CFG_STRING(net_host_identity, "net.host_identity", "net", "", "VOTVCOOP_NET_HOST_IDENTITY", false,
    "The host identity a client dials over signaling, copied from the host's own log line. "
    "Leave empty -- the master hands it to you when you join from the browser.")
CFG_STRING(net_stun, "net.stun", "net", "stun.l.google.com:19302", "VOTVCOOP_NET_STUN", false,
    "STUN server list for NAT discovery (host:port, comma-separated).")
CFG_STRING(net_turn, "net.turn", "net", "", "VOTVCOOP_NET_TURN", false,
    "Static TURN relay server list. Empty = none; the master mints ephemeral TURN "
    "credentials when a session needs a relay.")
CFG_STRING(net_turn_user, "net.turn_user", "net", "", "VOTVCOOP_NET_TURN_USER", false,
    "Static TURN username (dev / self-hosted relays only).")
CFG_STRING(net_turn_pass, "net.turn_pass", "net", "", "VOTVCOOP_NET_TURN_PASS", false,
    "Static TURN password (dev / self-hosted relays only).")
CFG_ENUM(net_ice, "net.ice", "net", "", "all|relay|disable|default", "VOTVCOOP_NET_ICE",
    "ICE candidate policy for p2p sessions. Unset/all = every path; relay = force the TURN "
    "relay; disable/default = engine defaults.")

// The receive-boundary novelty cap (coop/text/novelty_ledger.h). Since the ImGui
// 1.92 flip every codepoint in the repertoire is rasterisable on demand from chat
// text, which is the one attacker-controlled string in the process -- so a few
// hundred bytes of diverse UTF-8 can force thousands of FreeType rasterisations
// on every receiving peer, inside one frame. These bound the BURST, not the total
// (the ledger is monotone by design); pack exhaustion is answered by capacity, in
// ui/fonts.cpp's TexMax reasoning. Filed under [net] because what they police is
// an INBOUND PEER FIELD -- the refusal happens beside the strict UTF-8 decode,
// not at any draw call.
CFG_INT(net_novelty_budget, "net.novelty_budget", "net", 512, 0, 100000, nullptr,
    "Never-before-seen codepoints one peer may introduce per window (0 = no cap). "
    "A text field that would exceed it is refused whole.")
CFG_INT(net_novelty_window_ms, "net.novelty_window_ms", "net", 10000, 100, 3600000, nullptr,
    "Length of the novelty budget window, in milliseconds.")

// ---- [player] ---------------------------------------------------------------
CFG_IDENTITY(player_skin, "player_skin", "player",
    "Persisted body-skin choice, rolled from the starter list on first launch. Change it "
    "in-game (F1 menu > skins).")
CFG_FLAG(nameplate, "nameplate", "player", true, nullptr,
    "Show nameplates over other players (also toggleable in the F1 menu).")
CFG_STRING(nick_color, "nick_color", "player", "unset", nullptr, false,
    "Your nickname color as RRGGBB hex, or unset. Pick it in-game (F1 menu color picker).")

// ---- [ui] -------------------------------------------------------------------
// ui.scale range = the user-pref clamp; scale.cpp consumes THIS row's range AND
// default -- single source.
CFG_FLOAT(ui_scale, "ui.scale", "ui", 1.25f, 0.75, 1.75, nullptr,
    "Scale multiplier for the mod's UI panels.")
CFG_FLAG(warn_perf_mods, "warn.perf_mods", "ui", true, nullptr,
    "Warn at boot when OTHER mods known to cost frames are loaded (UE4SS Lua mods in Mods/mods.txt, Blueprint paks in Content/Paks/LogicMods). Measured 2026-08-29: that set cost ~75 vs ~120 fps on one machine while Multivoid itself measured at no detectable cost. Set 0 to silence.")
CFG_FLAG(ui_netstats, "ui.netstats", "ui", false, nullptr,
    "Show the network statistics overlay.")
CFG_FLAG(ui_chat_peer_actions, "ui.chat.peer_actions", "ui", true, nullptr,
    "Show peer action lines (joins, pickups, device use) in the chat feed.")
CFG_STRING(browser_lastdirect, "browser.lastdirect", "ui", "127.0.0.1:7777", nullptr, false,
    "The last address typed into the server browser's direct-connect box (remembered "
    "automatically).")
CFG_FLAG(multiplayer_menu_off, "multiplayer_menu_off", "ui", false, nullptr,
    "Set to 1 to hide the Multiplayer main-menu button (the mod stays loaded).")
// The composed ui.font.<role> family, per-role Enum rows. Default family per
// role = the user-2026-07-09 decision: menu/chat/toast = fixedsys(3),
// net/nameplate = roboto(1). Family index order = kFontFamilyTokens.
CFG_FONTROLE(ui_font_menu, "ui.font.menu", "menu", 3,
    "Font family for the mod's menus and panels.")
CFG_FONTROLE(ui_font_chat, "ui.font.chat", "chat", 3,
    "Font family for the chat feed.")
CFG_FONTROLE(ui_font_net, "ui.font.net", "net", 1,
    "Font family for the net-stats overlay.")
CFG_FONTROLE(ui_font_nameplate, "ui.font.nameplate", "nameplate", 1,
    "Font family for player nameplates.")
CFG_FONTROLE(ui_font_toast, "ui.font.toast", "toast", 3,
    "Font family for the update/version toast.")

// ---- [voice] ----------------------------------------------------------------
CFG_FLAG(voice_enabled, "voice.enabled", "voice", true, "VOTVCOOP_VOICE_ENABLED",
    "Master switch for voice chat (restart the game after changing).")
CFG_ENUM(voice_mode, "voice.mode", "voice", "ptt", "ptt|activation", "VOTVCOOP_VOICE_MODE",
    "Voice transmit mode: ptt = push-to-talk, activation = voice-activated.")
CFG_STRING(voice_ptt_key, "voice.ptt_key", "voice", "G", nullptr, false,
    "Push-to-talk key: a single letter, or a virtual-key number.")
CFG_STRING(voice_whisper_key, "voice.whisper_key", "voice", "", nullptr, false,
    "Whisper key (reduced voice range while held). Empty = disabled.")
CFG_STRING(voice_mute_key, "voice.mute_key", "voice", "", nullptr, false,
    "Microphone mute toggle key. Empty = disabled.")
CFG_FLOAT(voice_threshold_db, "voice.threshold_db", "voice", -50.0f, -120, 20, "VOTVCOOP_VOICE_THRESHOLD_DB",
    "Voice-activation threshold in dB: speech louder than this transmits (activation mode).")
CFG_FLOAT(voice_mic_gain_db, "voice.mic_gain_db", "voice", 0.0f, -60, 60, nullptr,
    "Microphone gain in dB applied before sending.")
CFG_STRING(voice_mic_device, "voice.mic_device", "voice", "", nullptr, false,
    "Capture device name; empty = system default. Pick it via the in-game voice panel.")
CFG_STRING(voice_output_device, "voice.output_device", "voice", "", nullptr, false,
    "Playback device name; empty = system default. Pick it via the in-game voice panel.")
CFG_FLOAT(voice_volume, "voice.volume", "voice", 1.0f, 0, 10, nullptr,
    "Master playback volume multiplier for incoming voice.")
CFG_FLOAT(voice_distance_cm, "voice.distance_cm", "voice", 4800.0f, 100, 1000000, nullptr,
    "Maximum voice audibility distance in centimeters (4800 = 48 m).")
CFG_INT(voice_jitter_threshold, "voice.jitter_threshold", "voice", 3, 0, 60, nullptr,
    "Jitter buffer: late-packet tolerance in frames before a reset.")
CFG_INT(voice_prebuffer_frames, "voice.prebuffer_frames", "voice", 5, 0, 60, nullptr,
    "Jitter buffer: frames buffered before playback starts.")
CFG_FLAG(voice_test_tone, "voice.test_tone", "voice", false, "VOTVCOOP_VOICE_TEST_TONE",
    "Dev: replace the microphone with a generated test tone.")
CFG_FLAG(voice_loopback, "voice.loopback", "voice", false, "VOTVCOOP_VOICE_LOOPBACK",
    "Dev: play your own transmitted voice back locally.")

// ---- [death] ----------------------------------------------------------------
// KO RESPAWN (2026-08): instead of the stock permadeath (death -> kick to the main
// menu -> rejoin), lethal damage knocks the player out (a faint/ragdoll) and, after
// ko_ragdoll_seconds, respawns them with full vitals. Items are kept. The lethal hit
// is intercepted at AddPlayerDamage; fall/fire/radiation deaths that bypass it are
// caught by the net_pump death backstop and converted the same way.
CFG_FLAG(ko_respawn, "death.ko_respawn", "death", true, "VOTVCOOP_KO_RESPAWN",
    "Lethal damage knocks the player out instead of killing them (KO ragdoll, then a "
    "respawn with full vitals). 0 = the stock permadeath flow (kick to the main menu).")
CFG_INT(ko_ragdoll_seconds, "death.ko_ragdoll_seconds", "death", 5, 1, 30, "VOTVCOOP_KO_RAGDOLL_SECONDS",
    "How long a KO'd player lies ragdolled before the respawn fires.")
CFG_INT(ko_invulnerable_seconds, "death.ko_invulnerable_seconds", "death", 3, 0, 60, "VOTVCOOP_KO_INVULNERABLE_SECONDS",
    "Damage immunity after a KO respawn (all damage is canceled during it).")
CFG_FLAG(ko_spawn_at_start, "death.ko_spawn_at_start", "death", true, "VOTVCOOP_KO_SPAWN_AT_START",
    "Respawn at the КПП start point. 0 = get up where the player fell.")

// ---- [dev] ------------------------------------------------------------------
// enabled: the master kill-switch -- ABSENT/garbage default TRUE (= granular
// switches decide); only an explicit falsy forces dev features off.
CFG_FLAG(enabled, "enabled", "dev", true, nullptr,
    "Master switch for ALL dev/test features below: an explicit 0 disables them; absent or "
    "1 lets the granular switches decide.")
CFG_FLAG(devkeys, "devkeys", "dev", false, nullptr,
    "Enable the developer hotkeys.")
CFG_STRING(save, "save", "dev", "s_may2026", "VOTVCOOP_SAVE", false,
    "Test framework: the save slot name loaded at auto-start.")
CFG_FLAG(fresh_boot, "fresh_boot", "dev", false, "VOTVCOOP_FRESH",
    "Test framework: boot to a fresh new-game state instead of loading a save.")
CFG_FLAG(static_2nd_player, "static_2nd_player", "dev", false, nullptr,
    "Spawn a static second-player puppet for camera/pose testing.")
CFG_FLAG(freecam, "freecam", "dev", false, nullptr,
    "Enable the freecam toggle.")
CFG_FLAG(spawn_menu_unlock, "spawn_menu_unlock", "dev", false, nullptr,
    "Unlock the native spawn menu while in a coop session.")
CFG_FLAG(desk_diag, "desk_diag", "dev", false, nullptr,
    "Diagnostic: periodically log the signal-desk state.")
CFG_INT(desk_diag_ms, "desk_diag_ms", "dev", 1000, 100, 60000, nullptr,
    "Interval in milliseconds for the desk_diag log.")
CFG_INT(vitals_keepalive_sec, "vitals_keepalive_sec", "dev", 0, 0, 86400, nullptr,
    "Reset player vitals every N seconds (0 = off; long unattended test runs).")
CFG_FLAG(window_log, "window_log", "dev", false, nullptr,
    "Diagnostic logging for window sync.")
CFG_FLAG(window_synth, "window_synth", "dev", false, nullptr,
    "Test driver: synthesize window interactions.")
CFG_FLAG(weather_observer_log, "weather_observer_log", "dev", false, nullptr,
    "Diagnostic logging for the weather sync observers.")
CFG_FLAG(weather_probe, "weather_probe", "dev", false, nullptr,
    "Diagnostic probe block for the weather subsystem.")
CFG_FLAG(meadow_selftest, "meadow_selftest", "dev", false, nullptr,
    "Autonomous selftest: meadow signal database sync.")
CFG_FLAG(laptop_selftest, "laptop_selftest", "dev", false, nullptr,
    "Autonomous selftest: laptop buffer sync.")
CFG_FLAG(deck_selftest, "deck_selftest", "dev", false, nullptr,
    "Autonomous selftest: tape deck playback sync.")
CFG_FLAG(drive_selftest, "drive_selftest", "dev", false, nullptr,
    "Autonomous selftest: drive rack digest instrument.")
CFG_FLAG(desk_snd_selftest, "desk_snd_selftest", "dev", false, nullptr,
    "Autonomous selftest: desk sound mirror.")
CFG_FLAG(container_selftest, "container_selftest", "dev", false, nullptr,
    "Autonomous selftest: container contents sync.")
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
CFG_FLAG(grime_log, "grime_log", "dev", false, nullptr,
    "Diagnostic logging for grime/cleaning sync.")
CFG_FLAG(interactable_log, "interactable_log", "dev", false, nullptr,
    "Diagnostic logging for the interactable channel.")
CFG_FLAG(client_model_probe, "client_model_probe", "dev", false, nullptr,
    "Diagnostic probe: client-side model/skin state.")
CFG_FLAG(delivery_census, "delivery_census", "dev", false, nullptr,
    "Diagnostic census: delivery spawn events.")
CFG_FLAG(store_table_probe, "store_table_probe", "dev", false, nullptr,
    "Diagnostic census: which mechanism can read a list_store row (one-shot).")
CFG_FLAG(order_selftest, "order_selftest", "dev", false, nullptr,
    "Selftest: a client places a real shop order so host-side pricing is exercised.")
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
    "ATV probe SIT arm (host, one shot): seat the local player so the ATV is authored.")
CFG_FLAG(native_pile_inert_probe, "native_pile_inert_probe", "dev", false, nullptr,
    "Diagnostic probe: native pile inertness on clients.")
CFG_FLAG(inventory_probe, "inventory_probe", "dev", false, nullptr,
    "Diagnostic probe: inventory store contents.")
CFG_FLAG(spawn_order_probe, "spawn_order_probe", "dev", false, nullptr,
    "Diagnostic probe: actor spawn ordering.")
CFG_FLAG(eid_lifetime_trace, "eid_lifetime_trace", "dev", false, nullptr,
    "Diagnostic trace: entity-id lifetimes (mint/adopt/destroy).")
CFG_FLAG(pinecone_probe, "pinecone_probe", "dev", false, nullptr,
    "Diagnostic probe: pinecone prop behavior.")
CFG_FLAG(live_store_readout, "live_store_readout", "dev", false, nullptr,
    "Diagnostic readout: the live GObjStack container store.")
CFG_FLAG(heap_probe, "heap_probe", "dev", false, nullptr,
    "Diagnostic probe: process heap usage.")
CFG_FLAG(drone_probe, "drone_probe", "dev", false, nullptr,
    "Diagnostic probe: delivery drone state.")
CFG_FLAG(drone_probe_drive, "drone_probe_drive", "dev", false, nullptr,
    "Diagnostic probe: drive the delivery drone (test driver).")
CFG_FLAG(pile_dup_probe, "pile_dup_probe", "dev", false, nullptr,
    "Diagnostic probe: pile duplicate/identity mismatches.")
CFG_FLAG(perf_probe, "perf_probe", "dev", false, nullptr,
    "Performance probe: per-frame subsystem timing lines.")
CFG_FLAG(perf_probe_selftime, "perf_probe_selftime", "dev", false, nullptr,
    "Performance probe: self-time breakdown variant.")
CFG_FLAG(perf_probe_dispatch, "perf_probe_dispatch", "dev", false, nullptr,
    "Performance probe: count every ProcessEvent dispatch (PE/s, game-thread share, our share). OFF by default -- gameplay dispatches ~170k/s across several threads, so this is a shared-atomic RMW on a contended cache line at that rate. Leave it off whenever the number being measured is what the mod itself costs; the 1 Hz fps line does not need it.")
CFG_INT(perf_probe_resdrop, "perf_probe_resdrop", "dev", 0, 0, 3600, nullptr,
    "Performance probe: after N 1 Hz samples, collapse render resolution (r.ScreenPercentage 25) to separate a GPU-bound frame from a CPU-bound one.")
CFG_INT(perf_probe_bypass, "perf_probe_bypass", "dev", 0, 0, 3600, nullptr,
    "Performance probe: after N samples, arm the transparent ProcessEvent bypass for 5 s (separates our dispatch-path cost from everything else about our presence).")
CFG_INT(perf_probe_statunit, "perf_probe_statunit", "dev", 0, 0, 3600, nullptr,
    "Performance probe: after N samples, issue `stat unit` + r.VSync 0 + t.MaxFPS 0. OFF by default because enabling any UE4 stat turns on FThreadStats collection engine-wide -- an instrument that changes the frame it measures. The delay exists so the samples before it are the uninstrumented baseline and the step across this line IS the instrument's own cost.")
CFG_FLAG(gnatives_probe, "gnatives_probe", "dev", false, nullptr,
    "Diagnostic probe: GNatives verb dispatch measurement.")
CFG_FLAG(gnatives_probe_disabled, "gnatives_probe_disabled", "dev", false, nullptr,
    "Variant of gnatives_probe with interception disabled (control run).")
CFG_FLAG(lightswitch_probe, "lightswitch_probe", "dev", false, nullptr,
    "Diagnostic probe: light switch interactions.")
CFG_FLAG(sleep_probe, "sleep_probe", "dev", false, nullptr,
    "Diagnostic probe: sleep/wake cycle.")
CFG_FLAG(vm_dispatch_log, "vm_dispatch_log", "dev", false, nullptr,
    "Diagnostic logging for the BP-VM verb dispatch layer.")
CFG_FLAG(door_probe, "door_probe", "dev", false, nullptr,
    "Diagnostic probe: door state and suppression.")
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

// THE TWO ATLAS DRILL KNOBS. ui/atlas_watch.cpp ships two silent-degradation
// detectors, and a detector nobody has watched go RED is decoration
// ([[lesson-an-instrument-never-shown-failing-passes-by-construction]]). Each of
// these forces the exact condition one detector exists to catch, so the drill is
// a launch away instead of a source edit plus a rebuild -- which is the
// difference between a drill that gets re-run after the next refactor and one
// that does not. Both default to OFF and neither is reachable from any UI.
CFG_INT(atlas_texmax_drill, "atlas_texmax_drill", "dev", 0, 0, 8192,
    "VOTVCOOP_ATLAS_TEXMAX",
    "Override the font atlas size ceiling (0 = the shipped 2048). Set 256 to starve "
    "the packer and prove the pack-failure detector fires.")
// The admission drill (v144). CLIENT-SIDE ONLY, and that is the whole design:
// the host's gate has no knob, no dev branch and no env check anywhere, so the
// drill exercises the REAL refusal or it proves nothing. A knob on the host would
// make the drill's own pass conditional on a bypass existing in shipped code.
CFG_ENUM(auth_drill, "auth_drill", "dev", "off", "off|corrupt|silent",
    "VOTVCOOP_AUTH_DRILL",
    "Sabotage THIS CLIENT's identity proof, to prove the host refuses it. corrupt = "
    "send a flipped signature; silent = open the exchange and never answer. Both must "
    "leave this peer unseated and with no save. Drill only.")
CFG_FLAG(atlas_no_exclude_drill, "atlas_no_exclude_drill", "dev", false,
    "VOTVCOOP_ATLAS_NO_EXCLUDE",
    "Drop GlyphExcludeRanges from every font source, so the faces bake their whole "
    "cmaps. Proves the superset invariant fires. Breaks name folding -- drill only.")

// THE NATIVE-UI PROBE (docs/MULTIPLAYER_UI.md section 8, P1). Two knobs on
// purpose: the read half answers every question the native server browser's
// design left unmeasured and touches nothing, while RUNG 1 -- the one that
// decides the screen's PLACEMENT -- writes into the live shipped
// `switcher_widgets`. A write into the one hands-on-verified native inject does
// not ride the same opt-in as a log line.
CFG_FLAG(native_ui_probe, "native_ui_probe", "dev", false, "VOTVCOOP_NATIVE_UI_PROBE",
    "Diagnostic probe (READ-ONLY): UMG class/function resolution census, donor "
    "residency, the ui_menu switcher child map, and the count of frames presented "
    "while no world exists (decides whether the ImGui overlay substrate is retirable).")
CFG_FLAG(native_ui_probe_write, "native_ui_probe_write", "dev", false,
    "VOTVCOOP_NATIVE_UI_PROBE_WRITE",
    "RUNG 1 of native_ui_probe, and it WRITES: adds a throwaway UUserWidget to the "
    "live ui_menu widget switcher, shows it for ~2 s, then restores the previous "
    "active index and removes it. Main menu only, never with a session up. Requires "
    "native_ui_probe=1.")
// The NATIVE server browser (docs/MULTIPLAYER_UI.md section 8, P2). Built dark: while
// this is off the screen is never constructed and the shipped ImGui browser is the only
// browser, so there is never a moment when two of them serve the same button.
CFG_FLAG(browser_native, "browser_native", "dev", false, "VOTVCOOP_BROWSER_NATIVE",
    "Build the native UMG server browser into the main menu's widget switcher. Off "
    "ships the ImGui browser unchanged; this is the in-progress native replacement.")
CFG_FLAG(browser_autoopen, "browser_autoopen", "dev", false, "VOTVCOOP_BROWSER_AUTOOPEN",
    "Show the native browser as soon as it is built, without a click -- an autonomous "
    "run cannot press the MULTIPLAYER button. Requires browser_native=1.")
