// coop/config/config_registry_rows_dev_join.inc -- the dev rows of a session's join, rejoin and leave:
// the host knobs that hold a join's window open (the bracket, the world stream, the save request), the
// client drills that join from a world, at the host, again, or leave through the pause menu, and the
// probes of a joiner's world load. Included from config_registry_rows_dev.inc the way _dev_port.inc is, so each
// row keeps its place in every expansion of the list. Entry forms are config_registry_rows.inc's.

CFG_INT(hold_snapshot_sec, "hold_snapshot_sec", "dev", 0, 0, 600,
    "VOTVCOOP_DEV_HOLD_SNAPSHOT_SEC",
    "Host testing knob: defer each joiner's world bracket for N seconds before sending it "
    "(0 = off). Reproduces the one host-side wait nothing else can stage -- a joiner with a "
    "healthy link and a world, waiting on a bracket the host is holding. Under the joiner's "
    "budget the join still completes; over it, it fails naming the host's phase.")
CFG_FLAG(join_draining_freeze, "join_draining_freeze", "dev", false,
    "VOTVCOOP_JOIN_DRAINING_FREEZE",
    "Host testing knob: while a joiner's bracket drains, the host keeps beaconing its Draining phase "
    "but with the first delivered-KiB value it sent, frozen for the rest of that drain. The joiner "
    "then sees a flat number and must end the wait at its 30 s budget, as it would for a link that "
    "stopped delivering. Off, the beacon carries the live count.")
CFG_INT(key_index_seed_hold_sec, "key_index_seed_hold_sec", "dev", 0, 0, 600,
    "VOTVCOOP_DEV_KEY_INDEX_SEED_HOLD_SEC",
    "Client testing knob: the prop key index seeds N seconds after the session starts (0 = off), so the "
    "joiner's world-ready announce waits on it. On a healthy run the index is seeded before the announce "
    "asks, so this is the one way to see that wait named in the log, then cleared.")
CFG_FLAG(key_index_refuse_install, "key_index_refuse_install", "dev", false,
    "VOTVCOOP_DEV_KEY_INDEX_REFUSE_INSTALL",
    "Testing knob: the prop key index's install reads as unable to index on this peer, so every session it "
    "would start is refused with MV-J32 before anything starts (a join through its connect-failed notice, a "
    "host on its status line). The watches and the script gate are untouched; it stages the refusal a build "
    "that cannot hook the game would meet.")
CFG_INT(stall_world_stream_at_pct, "stall_world_stream_at_pct", "dev", 0, 0, 99,
    "VOTVCOOP_DEV_STALL_WORLD_STREAM_AT_PCT",
    "Host testing knob: stop feeding a joiner's world blob once N% of it has been handed to the "
    "transport (0 = off). The connection stays healthy and the host keeps beaconing its phase, "
    "so this is a download that STOPS rather than a link that dies -- the shape a fixed transfer "
    "cap used to answer by booting a fresh world and finishing the join in it. The joiner's "
    "download budget now names it instead.")
CFG_ENUM(pause_quit_drill, "pause_quit_drill", "dev", "off", "off|client|host",
    "VOTVCOOP_PAUSE_QUIT_DRILL",
    "Developer: the named peer opens the pause menu and presses Main menu, as a player does, once it "
    "is in the session; prints [PAUSE-QUIT] lines.")
CFG_FLAG(quit_decision_red, "quit_decision_red", "dev", false, "VOTVCOOP_QUIT_DECISION_RED",
    "Developer: the quit decision does NOT end the session (the pause-quit drill's red: the menu "
    "world's destroys go out).")
CFG_FLAG(save_wait_probe, "save_wait_probe", "dev", false, "VOTVCOOP_SAVE_WAIT_PROBE",
    "Probe: the host holds each save request as if its world were still loading, for a second after it is in.")
CFG_FLAG(join_window_pos_trace, "join_window_pos_trace", "dev", false, nullptr,
    "Diagnostic trace: entity positions during the join window.")
CFG_FLAG(load_reroll_watch, "load_reroll_watch", "dev", false, "VOTVCOOP_LOAD_REROLL_WATCH",
    "Diagnostic probe (BOTH peers): what a world load rolls again. Watches the server-upgrade "
    "spawner and the coordinate towers' scramble from before a joining client's world exists, "
    "since both run inside the load, and logs the first calls with their callers. It holds the "
    "script gate on while set, with or without a session, so every lane's gate watch also runs "
    "outside a session for that run; read-only otherwise.")
CFG_STRING(radio_tower_probe, "radio_tower_probe", "dev", "", "VOTVCOOP_RADIO_TOWER_PROBE", false,
    "Diagnostic probe (BOTH peers): the radio tower. observe: reads the tower once its gamemode holds it (where, "
    "its puzzle's size, its fuse and puzzle counts), prints its state once a second when it changed, and counts "
    "each call of its bodies on this peer. fixclient: observe, plus a fixture: the host breaks the tower once a "
    "client's world is ready and the client completes the puzzle on its own copy, which today stays on the "
    "client. Holds the script gate on while set. Lines tagged [RADIO-TOWER]; the host's DONE line ends it.")
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
CFG_ENUM(join_abort, "join_abort", "dev", "off", "off|download|world",
    "VOTVCOOP_JOIN_ABORT",
    "Drill (CLIENT): press the loading screen's Cancel mid-join and prove the session-end seam tore "
    "the coop state down. 'download' (window A, before the pawn): run with the host's "
    "stall_world_stream_at_pct so the transfer cannot finish, press once the world blob is streaming "
    "with this client's apply blob held, and judge no apply blob survived the abort -- else an aborted "
    "join's inventory bleeds into the next join. 'world' (window B, after world-ready): run with the "
    "host's hold_snapshot_sec, press after this client announced world-ready, judge the announce state "
    "is cleared and that the player left the host's world for the menu, where the browser opens, then "
    "rejoin and pass at the second world-ready. [JOIN-ABORT] lines; download has no "
    "rejoin leg (the host's stall hits every joiner).")
CFG_FLAG(replay_gate_break, "replay_gate_break", "dev", false, "VOTVCOOP_DEV_REPLAY_GATE_BREAK",
    "Red arm: the joiner's world-ready probe opens without waiting for the save replay.")
CFG_FLAG(replay_watch_break, "replay_watch_break", "dev", false, "VOTVCOOP_DEV_REPLAY_WATCH_BREAK",
    "Red arm: the save-replay watch is registered on a function this game does not have.")
CFG_FLAG(replay_post_drop, "replay_post_drop", "dev", false, "VOTVCOOP_DEV_REPLAY_POST_DROP",
    "Red arm: the save-replay watch ignores the replay.")
CFG_FLAG(host_settle_red, "host_settle_red", "dev", false, "VOTVCOOP_HOST_SETTLE_RED",
    "Red arm (HOST): the host's world counts as settled as soon as a gamemode exists, the old test, for "
    "the join's hold and the start-up birth gate alike. The drill's real-conditions read "
    "is never overridden, so it names the capture that ran too early.")
CFG_FLAG(host_settle_drill, "host_settle_drill", "dev", false, "VOTVCOOP_HOST_SETTLE_DRILL",
    "Drill (HOST judges): a join that meets the host's world start-up waits for it. At the join's "
    "capture it reads the real conditions (the prop processor's phases and every container's "
    "inventory, never overridden by host_settle_red); it requires a request that waited in the hold, "
    "and no births dropped at the watcher's cap, then prints PASS, "
    "INCONCLUSIVE (the join never met the start-up) or FAIL, and DONE. Lines tagged [HOST-SETTLE-DRILL].")
CFG_FLAG(host_settle_ceiling_probe, "host_settle_ceiling_probe", "dev", false,
    "VOTVCOOP_HOST_SETTLE_CEILING_PROBE",
    "Probe (HOST): the settle walk never passes, as if the prop processor never finished, so the "
    "25 s ceiling admits the world with its WARN and the held join completes.")
CFG_STRING(new_game_rules, "new_game_rules", "dev", "", "VOTVCOOP_NEW_GAME_RULES", false,
    "Drill (any peer that boots a fresh New Game): world rules the blank save starts with, as the new-game "
    "menu's toggles write them. A list of key=value separated by ';' over the game's rule names (a bool is "
    "0 or 1, an enum its ordinal, a float its number), e.g. enableLightsOut=1;difficulty=2. A rule the "
    "game does not have is logged and skipped. A world loaded from a save is never touched.")
CFG_FLAG(atv_adopt_break,"atv_adopt_break", "dev", false, "VOTVCOOP_DEV_ATV_ADOPT_BREAK",
    "Red arm: an announced ATV is never adopted from the world's own copy; a mirror is spawned beside it.")
CFG_FLAG(atv_set_break, "atv_set_break", "dev", false, "VOTVCOOP_DEV_ATV_SET_BREAK",
    "Red arm: the host's complete ATV set removes nothing on this client.")

// The late-join drill's row: the host's staged world, and the bind wait's per-kind tallies on a client once its join
// has settled.
CFG_FLAG(late_join_drill, "late_join_drill", "dev", false, "VOTVCOOP_LATE_JOIN_DRILL",
    "Drill (both peers): the HOST stages a PC with its lid open, a floppy box, a Kerfus on, a rack with rows and a "
    "seated drive once its own world is up, before any client joins; the CLIENT (one life, no rejoin), once its join "
    "returns to idle, checks the staged world is in its world, prints the bind wait's tallies per message kind and in "
    "all (held, coalesced, replayed, dropped, given up, never bound) then and ten seconds later, judges the staged "
    "effects, then prints 'client DONE'. Lines tagged [LATE-JOIN].")
