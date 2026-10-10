// coop/config/config_registry_rows.inc -- the one registry row list, X-macro'd: config_registry.h expands it into
// the typed handle declarations and the font-role defaults, and config_registry.cpp expands it seven times (index
// enum, row table, font-role row indices, row flags, row labels, handle definitions, font-role handle array), so
// every artifact comes from one list with no transcription. The entry forms (ident is the C++ handle name, key
// the canonical ini spelling, desc the catalog's human text, semantics only, since the allowed tokens, ranges and
// env twin are generator-emitted from the columns): CFG_FLAG (ident, key, section, default, envVar, desc);
// CFG_INT and CFG_FLOAT add a range; CFG_ENUM takes the token list, an empty default being the unset sentinel
// allowed only for the role and ICE rows; CFG_STRING adds the seeded flag; CFG_IDENTITY is minted with no
// default; CFG_FONTROLE takes the role suffix and the default family index. A default is either the value the
// code used before the registry or an alias of the one owning constant. CFG_ROWFLAGS(ident, flags) marks the row
// `ident` with RowFlag bits, by convention directly under it, as Source declares FCVAR_REPLICATED with the cvar
// (config_registry.h). CFG_LABEL(ident, "text") gives the row `ident` a plain-English name for a generated pane
// (a row without one is never drawn); directly under the row, beside its CFG_ROWFLAGS.

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
CFG_ROWFLAGS(net_peer, kRowAddress)
CFG_STRING(net_server, "net.server", "net", "", "VOTVCOOP_NET_SERVER", false,
    "The server this install hosts: the name of its folder under multivoid_servers beside the game. "
    "Set from your nickname the first time you host; empty means you have not hosted yet.")
CFG_INT(net_port, "net.port", "net", ::coop::net::kDefaultPort, 1, 65535, "VOTVCOOP_NET_PORT",
    "UDP port a DIRECT host listens on: the lan topology, and a DIRECT game hosted from the menu.")
CFG_ENUM(net_topology, "net.topology", "net", "lan", "lan|p2p", "VOTVCOOP_NET_TOPOLOGY",
    "Transport shape: lan = direct UDP, p2p = internet session via the signaling server "
    "(NAT traversal, no port forwarding needed).")
CFG_INT(net_sendbuf_kb, "net.sendbuf_kb", "net", 0, 0, 262144, "VOTVCOOP_NET_SENDBUF_KB",
    "Per-connection network send buffer, KB. 0 = the mod's default. Testing knob -- lowering "
    "it forces send-buffer backpressure (the delivery-queue drill).")
CFG_INT(net_sendrate_kbs, "net.sendrate_kbs", "net", 0, 0, 1048576, "VOTVCOOP_NET_SENDRATE_KBS",
    "Fixed network send rate, KB/s (sets the transport's min+max rate clamp). 0 = the mod's "
    "default. Testing knob -- lowering it simulates a slow link end-to-end.")
CFG_FLAG(net_ratecontrol, "net.ratecontrol", "net", true, "VOTVCOOP_NET_RATECONTROL",
    "Whether the mod paces each connection at a rate it MEASURES. On by default, and the only "
    "thing that writes a send rate: nothing is pinned globally any more. Turning it OFF is a "
    "drill's control arm, not a fallback to the old behaviour -- with no writer at all the "
    "transport runs every link at its own stock 256 KB/s for the session's life. "
    "net.sendrate_kbs above overrides both.")
CFG_INT(net_fakelink_kbs, "net.fakelink_kbs", "net", 0, 0, 1048576, "VOTVCOOP_NET_FAKELINK_KBS",
    "Simulated OUTBOUND link capacity, KB/s (GNS FakeRateLimit_Send: a process-global policer "
    "that silently DROPS packets beyond the budget -- worst-case thin-uplink physics). 0 = off. "
    "Testing knob for the overdrive drill; pair with net.sendrate_kbs above this value.")
CFG_INT(net_fakereorder_pct, "net.fakereorder_pct", "net", 0, 0, 100, "VOTVCOOP_NET_FAKEREORDER_PCT",
    "Percent of RECEIVED packets held back by net.fakereorder_ms (GNS FakePacketReorder_Recv, "
    "process-global), so the packets behind them arrive first: a reordering link, which a LAN "
    "never is. 0 = off. Testing knob for the newest-wins stream stores.")
CFG_INT(net_fakereorder_ms, "net.fakereorder_ms", "net", 50, 1, 1000, "VOTVCOOP_NET_FAKEREORDER_MS",
    "How long net.fakereorder_pct holds a received packet back, ms (GNS FakePacketReorder_Time).")
// The default is the token rather than the list it stands for (protocol.h kOfficialMasterSlots):
// a player who adds a master of their own keeps the official ones a later build ships instead
// of freezing the ones they copied.
CFG_STRING(net_masters, "net.masters", "net", "default", "VOTVCOOP_NET_MASTERS", false,
    "The master servers the server browser can list, comma-separated; the first is the one it "
    "opens on. default stands for this build's official masters (USA, then EU). Add your own as "
    "Label=host:port after it, e.g. default,Home=my.host:10443 (host:port means TLS, "
    "http://host:port a master without a certificate); a list without default replaces the "
    "official ones. A label is what the browser's tab says, up to 16 plain characters.")
CFG_ROWFLAGS(net_masters, kRowAddress)
CFG_STRING(net_master, "net.master", "net", "", nullptr, false,
    "Which master the server browser lists and where a game you host is listed: a label from "
    "net.masters. Written when you pick a tab in the browser; empty means the first one.")
CFG_ROWFLAGS(net_master, kRowAddress)
CFG_STRING(net_signaling, "net.signaling", "net", "", "VOTVCOOP_NET_SIGNALING", false,
    "Signaling (rendezvous) server for a P2P connection made WITHOUT a master (a test, or a "
    "host's gen: line). Empty means the chosen master's own. Games found in the server browser "
    "always use the one their master hands out.")
CFG_ROWFLAGS(net_signaling, kRowAddress)
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
CFG_ROWFLAGS(net_stun, kRowAddress)
CFG_STRING(net_turn, "net.turn", "net", "", "VOTVCOOP_NET_TURN", false,
    "Static TURN relay server list. Empty = none; the master mints ephemeral TURN "
    "credentials when a session needs a relay.")
CFG_ROWFLAGS(net_turn, kRowAddress)
CFG_STRING(net_turn_user, "net.turn_user", "net", "", "VOTVCOOP_NET_TURN_USER", false,
    "Static TURN username (dev / self-hosted relays only).")
CFG_STRING(net_turn_pass, "net.turn_pass", "net", "", "VOTVCOOP_NET_TURN_PASS", false,
    "Static TURN password (dev / self-hosted relays only).")
CFG_ENUM_FAILCLOSED(net_ice, "net.ice", "net", "all", "all|relay", "VOTVCOOP_NET_ICE",
    "What other players see of your address. all = every path, direct first; relay = only "
    "through a TURN relay (the lobby master's, or net.turn), so peers see the relay's address "
    "instead of yours, and joining a direct lobby or by address is refused, since that would "
    "show the host yours (hosting DIRECT still publishes your address). Any other value, or a "
    "multivoid.ini that cannot be read, refuses automatic sessions and every direct join until "
    "it is fixed.")

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
CFG_STRING(ui_language, "ui.language", "ui", "auto", "VOTVCOOP_UI_LANGUAGE", false,
    "The language of the mod's own interface: auto follows the Windows display language, or name "
    "a locale such as zh_CN. A language without a catalogue shows English. Takes effect at the "
    "next start.")
CFG_STRING(browser_lastdirect, "browser.lastdirect", "ui", ::coop::net::kDefaultDirectAddr, nullptr, false,
    "The last address typed into the server browser's direct-connect box (remembered "
    "automatically). Its default names the port a host actually listens on (net.port), "
    "not Unreal's 7777, which nothing in the mod has ever bound. Replacing the address "
    "while keeping the port is the natural edit.")
CFG_ROWFLAGS(browser_lastdirect, kRowAddress)
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
CFG_FLOAT(voice_distance_cm, "voice.distance_cm", "voice", 4800.0f, 0, 1000000, nullptr,
    L10N_MARK("Maximum voice audibility distance in centimeters (4800 = 48 m); 0 = unlimited (a whisper "
    "stays local); the host's setting in a session. A value of 1..99 is a near-mute range, the "
    "host's to choose."))
CFG_ROWFLAGS(voice_distance_cm, kRowServer | kRowReplicated | kRowLive | kRowNotify)
CFG_LABEL(voice_distance_cm, L10N_MARK("Voice range"))
CFG_FLAG(net_allow_other_builds, "net.allow_other_builds", "net", false, "VOTVCOOP_NET_ALLOW_OTHER_BUILDS",
    L10N_MARK("Let players whose Multivoid build differs from this server's join -- an unofficial build when this server runs "
    "the official one, the official build when this server runs a modified one. Applies to the next joins; players "
    "already in stay."))
CFG_ROWFLAGS(net_allow_other_builds, kRowServer | kRowReplicated | kRowLive | kRowNotify)
CFG_LABEL(net_allow_other_builds, L10N_MARK("Allow other builds"))
CFG_INT(voice_jitter_threshold, "voice.jitter_threshold", "voice", 3, 0, 60, nullptr,
    "Jitter buffer: late-packet tolerance in frames before a reset.")
CFG_INT(voice_prebuffer_frames, "voice.prebuffer_frames", "voice", 5, 0, 60, nullptr,
    "Jitter buffer: frames buffered before playback starts.")
CFG_FLAG(voice_test_tone, "voice.test_tone", "voice", false, "VOTVCOOP_VOICE_TEST_TONE",
    "Dev: replace the microphone with a generated test tone.")
CFG_FLAG(voice_loopback, "voice.loopback", "voice", false, "VOTVCOOP_VOICE_LOOPBACK",
    "Dev: play your own transmitted voice back locally.")

// The [dev] section, its own file: it grows with every drill, and the rows above are the product's.
#include "coop/config/config_registry_rows_dev.inc"
