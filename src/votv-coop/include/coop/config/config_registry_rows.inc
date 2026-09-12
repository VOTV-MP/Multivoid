// coop/config/config_registry_rows.inc -- the one registry row list, X-macro'd:
// config_registry.h expands it into the typed handle declarations, and config_registry.cpp
// expands it three times (the index enum, the row table, the handle definitions), so every
// artifact comes from one list with no transcription. The entry forms (ident is the C++ handle
// name, key the canonical ini spelling, desc the catalog's human text, semantics only, since
// the allowed tokens, ranges and env twin are generator-emitted from the columns): CFG_FLAG
// (ident, key, section, default, envVar, desc); CFG_INT and CFG_FLOAT add a range; CFG_ENUM
// takes the token list, an empty default being the unset sentinel allowed only for the role
// and ICE rows; CFG_STRING adds the seeded flag; CFG_STRING_GATED adds the flag row key that
// gates the ini read; CFG_IDENTITY is minted with no default; CFG_FONTROLE takes the role
// suffix and the default family index. A default is either the value the code used before the
// registry or an alias of the one owning constant.

// The [net] section.
CFG_STRING(net_nick, "net.nick", "net", kMyNameDefault, "VOTVCOOP_NET_NICK", true,
    "Your display name, shown to other players (nameplates, chat, the scoreboard).")
// The lobby password, remembered between sessions so a host who has told friends one value
// need not tell them a new one every time. Written by the hosting window, which mints one the
// moment the player chooses a password and leaves it editable. It has an env twin: an env var
// is visible to every process on the box, but so is the ini line, so excluding it would buy
// nothing against an attacker already running as this user, and it would leave an untestable
// lane, since the drill that proves a wrong password is refused has no other way to configure
// the two peers. Not seeded: a generated value written at first boot would mint a password
// for every player who never hosts, and a value nothing produced is what gets pasted into a
// bug report.
CFG_STRING(net_lobby_password, "net.lobby_password", "net", "", "VOTVCOOP_NET_LOBBY_PASSWORD",
    false,
    "Password for sessions you host with the lock on. Generated for you when you first turn "
    "the lock on; change it to anything you like. Only players you give it to can join.")
// The password offered when joining someone else's locked session, a different secret from
// the one above; separating them closed a real leak. As one row, the join fell back to the
// lobby password when no prompt had been filled in, and that row holds the password the
// player's own hosted sessions require, so a player who had ever hosted a locked lobby offered
// their own secret to any locked host reached on the direct, LAN or scripted lanes; the
// binding gate stops a stranger grinding it, but carrying the wrong secret to the right host
// is not what binding covers. Normal play never touches this: the browser's prompt supplies
// the value for the one join it was typed for and clears it on use. The row exists for the
// client with no prompt: a scripted, LAN or dedicated joiner.
CFG_STRING(net_join_password, "net.join_password", "net", "", "VOTVCOOP_NET_JOIN_PASSWORD",
    false,
    "Password to offer when you join a locked server, for setups with no UI to type it in. "
    "Leave empty -- the server browser asks you when you click Connect on a locked server.")
CFG_FLAG(net_lobby_locked, "net.lobby_locked", "net", false, "VOTVCOOP_NET_LOBBY_LOCKED",
    "Whether the sessions you host require the password above. Set from the hosting window; "
    "remembered so your next session starts the way your last one did.")
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
// The host identity's job doubled and the description followed. It was a routing value read
// on the internet branch alone, which host to rendezvous with over signaling; it is now also
// the anchor a joiner binds the answering key to, and the password proof refuses to be
// emitted without that binding, so it is read on the direct and LAN lanes too. A player given
// only an address is unbound: they can still join any open server and are refused by a
// locked one with a sentence saying why. A stale value here refuses every direct and LAN
// join, hence the sharper clear-it in the text.
CFG_STRING(net_host_identity, "net.host_identity", "net", "", "VOTVCOOP_NET_HOST_IDENTITY", false,
    "The host you expect to reach, as their `gen:` line -- needed only to join a "
    "PASSWORD-LOCKED server by direct address or on a LAN, where nothing else can tell you "
    "who is answering. Leave it empty otherwise, and CLEAR it when you stop using it: a "
    "leftover value makes every direct connection refuse, because the host that answers is "
    "not the one it names. Joining from the server browser never needs it.")
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

// The receive-boundary novelty cap (coop/text/novelty_ledger.h). Every codepoint in the
// repertoire is rasterisable on demand from chat text, the one attacker-controlled string in
// the process, so a few hundred bytes of diverse UTF-8 can force thousands of rasterisations
// on every receiving peer inside one frame. These bound the burst, not the total (the ledger
// is monotone by design); pack exhaustion is answered by capacity, in the fonts module. Filed
// under [net] because what they police is an inbound peer field: the refusal happens beside
// the strict UTF-8 decode, not at any draw call.
CFG_INT(net_novelty_budget, "net.novelty_budget", "net", 512, 0, 100000, nullptr,
    "Never-before-seen codepoints one peer may introduce per window (0 = no cap). "
    "A text field that would exceed it is refused whole.")
CFG_INT(net_novelty_window_ms, "net.novelty_window_ms", "net", 10000, 100, 3600000, nullptr,
    "Length of the novelty budget window, in milliseconds.")
// The host's connection cap at the accept edge (coop/net/connect_history): MTA's join-flood
// numbers, four per thirty seconds per address and then thirty seconds of refusal. Filed under
// [net] because it is a host policy on inbound connections; the refusal carries MV-H29.
CFG_INT(net_connect_cap, "net.connect_cap", "net", 4, 0,
    ::coop::net::connect_history::History::kMaxStamps, "VOTVCOOP_NET_CONNECT_CAP",
    "Host: connections one address may open per window before it is refused for one window "
    "(0 = no cap). On the internet lane no address is known when a connection arrives, so it "
    "is counted at its identity proof, by the address its route reports or by the identity it "
    "proves over a relay. A refused joiner sees code MV-H29.")
CFG_INT(net_connect_window_s, "net.connect_window_s", "net", 30, 1, 3600,
    "VOTVCOOP_NET_CONNECT_WINDOW_S",
    "Host: length of the connection-cap window, in seconds; the refusal lasts as long.")

// The [player] section.
CFG_IDENTITY(player_skin, "player_skin", "player",
    "Persisted body-skin choice, rolled from the starter list on first launch. Change it "
    "in-game (F1 menu > skins).")
CFG_FLAG(nameplate, "nameplate", "player", true, nullptr,
    "Show nameplates over other players (also toggleable in the F1 menu).")
CFG_STRING(nick_color, "nick_color", "player", "unset", nullptr, false,
    "Your nickname color as RRGGBB hex, or unset. Pick it in-game (F1 menu color picker).")

// The [ui] section. The scale range is the user-preference clamp; the scale module consumes
// this row's range and default, a single source.
CFG_FLOAT(ui_scale, "ui.scale", "ui", 1.25f, 0.75, 1.75, nullptr,
    "Scale multiplier for the mod's UI panels.")
CFG_FLAG(ui_netstats, "ui.netstats", "ui", false, nullptr,
    "Show the network statistics overlay.")
CFG_FLAG(ui_chat_peer_actions, "ui.chat.peer_actions", "ui", true, nullptr,
    "Show peer action lines (joins, pickups, device use) in the chat feed.")
CFG_STRING(browser_lastdirect, "browser.lastdirect", "ui", ::coop::net::kDefaultDirectAddr, nullptr, false,
    "The last address typed into the server browser's direct-connect box (remembered "
    "automatically). Its default names the port a host actually listens on (net.port), "
    "not Unreal's 7777, which nothing in the mod has ever bound. Replacing the address "
    "while keeping the port is the natural edit.")
CFG_FLAG(multiplayer_menu_off, "multiplayer_menu_off", "ui", false, nullptr,
    "Set to 1 to hide the Multiplayer main-menu button (the mod stays loaded).")
// The composed font family, per-role enum rows. The default family per role: the menus, chat
// and the toast use fixedsys, the net overlay and the nameplates use roboto; the family index
// order is the font family token list.
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

// The [voice] section.
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
CFG_FLAG(hand_drop_selftest, "hand_drop_selftest", "dev", false, "VOTVCOOP_HAND_DROP_SELFTEST",
    "Autonomous selftest: a prop through a player's hotbar HAND and back out -- pick it up with "
    "the game's own verb, drop it, first with the host holding and then with the client, twice "
    "each. The verdict is the watching peer's census of the keyed props around the holder before "
    "and after: a prop that does not come back is the release the other peer never received. It "
    "MUTATES the world, so it is armed per run from the environment.")
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
CFG_FLAG(pile_delta_probe, "pile_delta_probe", "dev", false, nullptr,
    "Diagnostic probe: trash pile delta events.")
CFG_FLAG(garbage_pickup_probe, "garbage_pickup_probe", "dev", false, nullptr,
    "Diagnostic probe: garbage pickup path.")
CFG_FLAG(world_actor_trace, "world_actor_trace", "dev", false, nullptr,
    "Diagnostic trace: the client-side world-actor mirror drive, once a second per mirror. Costs "
    "two extra location reads per mirror per second while on.")
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
    "ATV probe DRIVE arm (ONE peer, one shot): seat the local player, release the brake and hold the throttle so the ATV is authored AND moving. Set it on one peer only.")
CFG_FLAG(atv_corrector, "atv_corrector", "dev", true, "VOTVCOOP_ATV_CORRECTOR",
    "ATV pose corrector, ON by default. Set 0 to leave a mirrored ATV entirely alone -- the "
    "CONTROL arm for 'does the mirror settle wrong on its own, or do we settle it wrong'. The "
    "mirror still simulates and still receives state; only the position/velocity correction is "
    "withheld, so the two arms differ in exactly one thing.")
CFG_FLAG(native_pile_inert_probe, "native_pile_inert_probe", "dev", false, nullptr,
    "Diagnostic probe: native pile inertness on clients.")
CFG_FLAG(inventory_probe, "inventory_probe", "dev", false, nullptr,
    "Diagnostic probe: inventory store contents.")
CFG_FLAG(spawn_order_probe, "spawn_order_probe", "dev", false, nullptr,
    "Diagnostic probe: actor spawn ordering.")
CFG_FLAG(prop_birth_key_probe, "prop_birth_key_probe", "dev", false, nullptr,
    "Diagnostic probe (CLIENT): the place/birth seam -- how many drain ticks the save Key takes to "
    "restore after a spawn, and which of the seven drain exits each pending spawn leaves by. Set it "
    "on the CLIENT; the host places through a different path.")
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
CFG_FLAG(lightswitch_probe, "lightswitch_probe", "dev", false, "VOTVCOOP_LIGHTSWITCH_PROBE",
    "Diagnostic probe: light switch interactions + the light-group apply selftest. MUTATES the world (one-shot synthetic use()) -- never enable it on two peers at once. The env var exists so `mp.py lightgroup` can arm it: an ini-only dev row is unreachable to every launcher, which is how this selftest went unrun after a refactor.")
CFG_FLAG(lightgroup_census, "lightgroup_census", "dev", false, nullptr,
    "Diagnostic census: per-key light GROUP state (isActive / active gate / buffIsActive) + switch A. Read-only; enable on BOTH peers and diff by key.")
CFG_FLAG(sleep_probe, "sleep_probe", "dev", false, nullptr,
    "Diagnostic probe: sleep/wake cycle.")
CFG_FLAG(script_gate_log, "script_gate_log", "dev", false, nullptr,
    "Diagnostic logging for the script-body gate's consumers (the kerfur form assembler's per-verb lines).")
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
