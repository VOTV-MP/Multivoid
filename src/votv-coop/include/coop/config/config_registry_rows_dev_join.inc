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
