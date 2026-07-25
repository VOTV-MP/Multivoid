// coop/config/config_registry_rows.inc -- THE one registry row list (ini rework
// arc 3, T2-migrate). X-macro'd: config_registry.h expands it into the typed
// handle DECLARATIONS (rows::<ident>), config_registry.cpp expands it three
// times (index enum / Row table / handle definitions) -- ONE list, every
// artifact, no transcription (the drift class T2 exists to kill).
//
// Entry forms (ident = the C++ handle name; key = canonical ini spelling):
//   CFG_FLAG    (ident, key, section, defB, envVar)
//   CFG_INT     (ident, key, section, defI, lo, hi, envVar)
//   CFG_FLOAT   (ident, key, section, defF, lo, hi, envVar)
//   CFG_ENUM    (ident, key, section, defS, tokens, envVar)   // defS "" = the
//               // unset sentinel, allowed ONLY for net.role / net.ice
//               // (ValidateRows allowlist)
//   CFG_STRING  (ident, key, section, defS, envVar, seeded)
//   CFG_IDENTITY(ident, key, section)                          // minted, no def
//   CFG_FONTROLE(ident, key, roleSuffix, defFamilyIndex)       // Enum-kind row,
//               // tokens = the family list; ValidateRows pins key ==
//               // "ui.font." + roleSuffix AND roleSuffix == kFontRoleKeys[i]
//               // in order AND defS == kFontFamilyTokens[defFamilyIndex]
//
// DEFAULTS carry the arc-3 migration: each value here is the literal that lived
// at the call site through arc 2 (BEFORE harvest 2026-07-25: 100 keys, 108
// sites, ZERO same-key conflicts), or an ALIAS of the one owning constant
// (kDefaultPort / kOfficialMasterUrl / kOfficialSignalingUrl / kMyNameDefault)
// -- never a second copy of an owned value.

// ---- [net] ------------------------------------------------------------------
CFG_STRING(net_nick, "net.nick", "net", kMyNameDefault, "VOTVCOOP_NET_NICK", true)
CFG_ENUM(net_role, "net.role", "net", "", "host|client", "VOTVCOOP_NET_ROLE")
CFG_STRING(net_peer, "net.peer", "net", "127.0.0.1", "VOTVCOOP_NET_PEER", false)
CFG_INT(net_port, "net.port", "net", ::coop::net::kDefaultPort, 1, 65535, "VOTVCOOP_NET_PORT")
CFG_ENUM(net_topology, "net.topology", "net", "lan", "lan|p2p", "VOTVCOOP_NET_TOPOLOGY")
CFG_STRING(net_master, "net.master", "net", ::coop::net::kOfficialMasterUrl, "VOTVCOOP_MASTER_URL", false)
CFG_FLAG(net_master_custom, "net.master.custom", "net", false, nullptr)
CFG_STRING(net_signaling, "net.signaling", "net", ::coop::net::kOfficialSignalingUrl, "VOTVCOOP_NET_SIGNALING", false)
CFG_STRING(net_signaling_token, "net.signaling_token", "net", "", "VOTVCOOP_NET_SIGNALING_TOKEN", false)
CFG_STRING(net_identity, "net.identity", "net", "", "VOTVCOOP_NET_IDENTITY", false)
CFG_STRING(net_host_identity, "net.host_identity", "net", "", "VOTVCOOP_NET_HOST_IDENTITY", false)
CFG_STRING(net_stun, "net.stun", "net", "stun.l.google.com:19302", "VOTVCOOP_NET_STUN", false)
CFG_STRING(net_turn, "net.turn", "net", "", "VOTVCOOP_NET_TURN", false)
CFG_STRING(net_turn_user, "net.turn_user", "net", "", "VOTVCOOP_NET_TURN_USER", false)
CFG_STRING(net_turn_pass, "net.turn_pass", "net", "", "VOTVCOOP_NET_TURN_PASS", false)
CFG_ENUM(net_ice, "net.ice", "net", "", "all|relay|disable|default", "VOTVCOOP_NET_ICE")

// ---- [player] ---------------------------------------------------------------
CFG_IDENTITY(player_guid, "player_guid", "player")
CFG_IDENTITY(player_skin, "player_skin", "player")
CFG_FLAG(nameplate, "nameplate", "player", true, nullptr)
CFG_STRING(nick_color, "nick_color", "player", "unset", nullptr, false)

// ---- [ui] -------------------------------------------------------------------
// ui.scale range = the user-pref clamp; scale.cpp consumes THIS row's range AND
// (since arc 3) THIS row's default -- single source.
CFG_FLOAT(ui_scale, "ui.scale", "ui", 1.25f, 0.75, 1.75, nullptr)
CFG_FLAG(ui_netstats, "ui.netstats", "ui", false, nullptr)
CFG_FLAG(ui_chat_peer_actions, "ui.chat.peer_actions", "ui", true, nullptr)
CFG_STRING(browser_lastdirect, "browser.lastdirect", "ui", "127.0.0.1:7777", nullptr, false)
CFG_FLAG(multiplayer_menu_off, "multiplayer_menu_off", "ui", false, nullptr)
// The composed ui.font.<role> family, per-role rows (arc 3: born Enum-kind so
// the T10 sweep loud-reports garbage; the in-memory fallback is unchanged).
// Default family per role = the user-2026-07-09 decision (fonts.cpp's retired
// RoleDesc.defaultFam column): menu/chat/toast = fixedsys(3), net/nameplate =
// roboto(1). Family index order = kFontFamilyTokens.
CFG_FONTROLE(ui_font_menu, "ui.font.menu", "menu", 3)
CFG_FONTROLE(ui_font_chat, "ui.font.chat", "chat", 3)
CFG_FONTROLE(ui_font_net, "ui.font.net", "net", 1)
CFG_FONTROLE(ui_font_nameplate, "ui.font.nameplate", "nameplate", 1)
CFG_FONTROLE(ui_font_toast, "ui.font.toast", "toast", 3)

// ---- [voice] ----------------------------------------------------------------
CFG_FLAG(voice_enabled, "voice.enabled", "voice", true, "VOTVCOOP_VOICE_ENABLED")
CFG_ENUM(voice_mode, "voice.mode", "voice", "ptt", "ptt|activation", "VOTVCOOP_VOICE_MODE")
CFG_STRING(voice_ptt_key, "voice.ptt_key", "voice", "G", nullptr, false)
CFG_STRING(voice_whisper_key, "voice.whisper_key", "voice", "", nullptr, false)
CFG_STRING(voice_mute_key, "voice.mute_key", "voice", "", nullptr, false)
CFG_FLOAT(voice_threshold_db, "voice.threshold_db", "voice", -50.0f, -120, 20, "VOTVCOOP_VOICE_THRESHOLD_DB")
CFG_FLOAT(voice_mic_gain_db, "voice.mic_gain_db", "voice", 0.0f, -60, 60, nullptr)
CFG_STRING(voice_mic_device, "voice.mic_device", "voice", "", nullptr, false)
CFG_STRING(voice_output_device, "voice.output_device", "voice", "", nullptr, false)
CFG_FLOAT(voice_volume, "voice.volume", "voice", 1.0f, 0, 10, nullptr)
CFG_FLOAT(voice_distance_cm, "voice.distance_cm", "voice", 4800.0f, 100, 1000000, nullptr)
CFG_INT(voice_jitter_threshold, "voice.jitter_threshold", "voice", 3, 0, 60, nullptr)
CFG_INT(voice_prebuffer_frames, "voice.prebuffer_frames", "voice", 5, 0, 60, nullptr)
CFG_FLAG(voice_test_tone, "voice.test_tone", "voice", false, "VOTVCOOP_VOICE_TEST_TONE")
CFG_FLAG(voice_loopback, "voice.loopback", "voice", false, "VOTVCOOP_VOICE_LOOPBACK")

// ---- [dev] ------------------------------------------------------------------
// enabled: the master kill-switch -- ABSENT/garbage default TRUE (= granular
// switches decide); only an explicit falsy forces dev features off
// (MasterEnabled()'s measured arc-2 semantics, preserved by def=true).
CFG_FLAG(enabled, "enabled", "dev", true, nullptr)
CFG_FLAG(devkeys, "devkeys", "dev", false, nullptr)
CFG_STRING(save, "save", "dev", "s_may2026", "VOTVCOOP_SAVE", false)
CFG_FLAG(fresh_boot, "fresh_boot", "dev", false, "VOTVCOOP_FRESH")
CFG_FLAG(static_2nd_player, "static_2nd_player", "dev", false, nullptr)
CFG_FLAG(freecam, "freecam", "dev", false, nullptr)
CFG_FLAG(spawn_menu_unlock, "spawn_menu_unlock", "dev", false, nullptr)
CFG_FLAG(desk_diag, "desk_diag", "dev", false, nullptr)
CFG_INT(desk_diag_ms, "desk_diag_ms", "dev", 1000, 100, 60000, nullptr)
CFG_INT(vitals_keepalive_sec, "vitals_keepalive_sec", "dev", 0, 0, 86400, nullptr)
CFG_FLAG(window_log, "window_log", "dev", false, nullptr)
CFG_FLAG(window_synth, "window_synth", "dev", false, nullptr)
CFG_FLAG(weather_observer_log, "weather_observer_log", "dev", false, nullptr)
CFG_FLAG(weather_probe, "weather_probe", "dev", false, nullptr)
CFG_FLAG(meadow_selftest, "meadow_selftest", "dev", false, nullptr)
CFG_FLAG(laptop_selftest, "laptop_selftest", "dev", false, nullptr)
CFG_FLAG(deck_selftest, "deck_selftest", "dev", false, nullptr)
CFG_FLAG(drive_selftest, "drive_selftest", "dev", false, nullptr)
CFG_FLAG(desk_snd_selftest, "desk_snd_selftest", "dev", false, nullptr)
CFG_FLAG(container_selftest, "container_selftest", "dev", false, nullptr)
CFG_FLAG(inventory_selftest, "inventory_selftest", "dev", false, nullptr)
CFG_FLAG(starterkit_test, "starterkit_test", "dev", false, nullptr)
CFG_FLAG(reseed_orphan_selftest, "reseed_orphan_selftest", "dev", false, nullptr)
CFG_FLAG(pile_delta_probe, "pile_delta_probe", "dev", false, nullptr)
CFG_FLAG(garbage_pickup_probe, "garbage_pickup_probe", "dev", false, nullptr)
CFG_FLAG(grime_log, "grime_log", "dev", false, nullptr)
CFG_FLAG(interactable_log, "interactable_log", "dev", false, nullptr)
CFG_FLAG(client_model_probe, "client_model_probe", "dev", false, nullptr)
CFG_FLAG(delivery_census, "delivery_census", "dev", false, nullptr)
CFG_FLAG(flashlight_log, "flashlight_log", "dev", false, nullptr)
CFG_FLAG(kerfur_census, "kerfur_census", "dev", false, nullptr)
CFG_FLAG(object_overlay, "object_overlay", "dev", false, nullptr)
CFG_FLAG(ragdoll_bone_overlay, "ragdoll_bone_overlay", "dev", false, nullptr)
CFG_FLAG(join_window_pos_trace, "join_window_pos_trace", "dev", false, nullptr)
CFG_FLAG(puppet_head_probe, "puppet_head_probe", "dev", false, nullptr)
CFG_FLAG(native_pile_inert_probe, "native_pile_inert_probe", "dev", false, nullptr)
CFG_FLAG(inventory_probe, "inventory_probe", "dev", false, nullptr)
CFG_FLAG(spawn_order_probe, "spawn_order_probe", "dev", false, nullptr)
CFG_FLAG(eid_lifetime_trace, "eid_lifetime_trace", "dev", false, nullptr)
CFG_FLAG(pinecone_probe, "pinecone_probe", "dev", false, nullptr)
CFG_FLAG(live_store_readout, "live_store_readout", "dev", false, nullptr)
CFG_FLAG(heap_probe, "heap_probe", "dev", false, nullptr)
CFG_FLAG(drone_probe, "drone_probe", "dev", false, nullptr)
CFG_FLAG(drone_probe_drive, "drone_probe_drive", "dev", false, nullptr)
CFG_FLAG(pile_dup_probe, "pile_dup_probe", "dev", false, nullptr)
CFG_FLAG(perf_probe, "perf_probe", "dev", false, nullptr)
CFG_FLAG(perf_probe_selftime, "perf_probe_selftime", "dev", false, nullptr)
CFG_FLAG(gnatives_probe, "gnatives_probe", "dev", false, nullptr)
CFG_FLAG(gnatives_probe_disabled, "gnatives_probe_disabled", "dev", false, nullptr)
CFG_FLAG(lightswitch_probe, "lightswitch_probe", "dev", false, nullptr)
CFG_FLAG(sleep_probe, "sleep_probe", "dev", false, nullptr)
CFG_FLAG(vm_dispatch_log, "vm_dispatch_log", "dev", false, nullptr)
CFG_FLAG(door_probe, "door_probe", "dev", false, nullptr)
CFG_FLAG(rng_roll_census, "rng_roll_census", "dev", false, nullptr)
CFG_FLAG(leak_probe, "leak_probe", "dev", false, nullptr)
CFG_FLAG(force_chippile_unclaim, "force_chippile_unclaim", "dev", false, nullptr)
CFG_FLAG(disable_completeness_floor, "disable_completeness_floor", "dev", false, nullptr)
CFG_FLAG(keypad_probe, "keypad_probe", "dev", false, nullptr)
CFG_FLAG(keypad_synth, "keypad_synth", "dev", false, nullptr)
CFG_FLAG(save_identity_map_log, "save_identity_map_log", "dev", false, nullptr)
CFG_FLAG(save_identity_bind, "save_identity_bind", "dev", false, nullptr)
CFG_FLAG(force_kerfur_unmap, "force_kerfur_unmap", "dev", false, nullptr)
CFG_FLAG(force_save_churn, "force_save_churn", "dev", false, nullptr)
