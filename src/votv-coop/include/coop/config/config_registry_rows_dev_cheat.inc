// coop/config/config_registry_rows_dev_cheat.inc -- the [dev] row of the cheat tools drill. Included by
// config_registry_rows_dev.inc where the row stood, so it keeps its place in every expansion of the list. The entry
// forms are config_registry_rows.inc's.

CFG_ENUM(cheat_drill, "cheat_drill", "dev", "off", "off|on", "VOTVCOOP_CHEAT_DRILL",
    "Drill (BOTH peers): the host's and the players' cheat tools. The host spawns two crowbars, adds points and fires an "
    "event through the panels' calls and the client must see each; with net.allow_player_cheats off the client's three "
    "requests are all refused and move nothing; with it on the client's points, crowbars and event run on the host and "
    "mirror here, a story event and an unlisted prop are refused; reset, the next request is refused again. Each peer first "
    "opens the F1 panes it is offered. Lines tagged [CHEAT-DRILL]; the host's PASS or FAIL line is the verdict.")
CFG_FLAG(cheat_node_check_off, "cheat_node_check_off", "dev", false, "VOTVCOOP_CHEAT_NODE_CHECK_OFF",
    "Developer red arm: never set (HOST). 1 makes the host skip the per-player right multivoid.dev.local.cheats when it "
    "judges a cheat request, so a request from a player whose right the host withheld runs, and the cheat drill's "
    "withheld arm must FAIL.")
CFG_FLAG(cheat_open_check_off, "cheat_open_check_off", "dev", false, "VOTVCOOP_CHEAT_OPEN_CHECK_OFF",
    "Developer red arm: never set (HOST). 1 makes the host skip the rule for what a player may fire or spawn, so a "
    "request for an event that is not Safe, or for an explosive prop, runs, and the cheat drill's refusal arm must "
    "FAIL.")
