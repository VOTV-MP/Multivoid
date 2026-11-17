// coop/config/config_registry_rows_dev_cheat.inc -- the [dev] row of the cheat tools drill. Included by
// config_registry_rows_dev.inc where the row stood, so it keeps its place in every expansion of the list. The entry
// forms are config_registry_rows.inc's.

CFG_ENUM(cheat_drill, "cheat_drill", "dev", "off", "off|on", "VOTVCOOP_CHEAT_DRILL",
    "Drill (BOTH peers): the host's and the players' cheat tools. The host spawns two crowbars, adds points and fires an "
    "event through the panels' calls and the client must see each; with net.allow_player_cheats off the client's three "
    "requests are all refused and move nothing; with it on the client's points, crowbars and event run on the host and "
    "mirror here, a story event and an unlisted prop are refused; then the hostile arms (raw requests no panel makes, each "
    "refused with its reason; a ten-prop request drains the request bucket and the next is answered Rate; eight removals "
    "against 50000 leave 0; two requests in flight; a withheld right answered Off; Unstuck onto the NavMesh); reset, the "
    "next request is refused again. Each peer first opens the F1 panes it is offered. Lines tagged [CHEAT-DRILL]; the "
    "host's PASS, FAIL or INVALID line is the verdict, and a FAIL is said to the peer at once.")
CFG_FLAG(cheat_node_check_off, "cheat_node_check_off", "dev", false, "VOTVCOOP_CHEAT_NODE_CHECK_OFF",
    "Developer red arm: never set (HOST). 1 makes the host skip the per-player right multivoid.dev.local.cheats when it "
    "judges a cheat request, so a request from a player whose right the host withheld runs, and the cheat drill's "
    "withheld arm must FAIL.")
CFG_FLAG(cheat_open_check_off, "cheat_open_check_off", "dev", false, "VOTVCOOP_CHEAT_OPEN_CHECK_OFF",
    "Developer red arm: never set (HOST). 1 makes the host skip the rule for what a player may fire or spawn, so a "
    "request for an event that is not Safe, or for an explosive prop, runs, and the cheat drill's refusal arm must "
    "FAIL.")
CFG_FLAG(cheat_count_cost_off, "cheat_count_cost_off", "dev", false, "VOTVCOOP_CHEAT_COUNT_COST_OFF",
    "Developer red arm: never set (BOTH peers). 1 makes a request for props cost one token whatever its count, on the "
    "host's judge and on the client's mirror, so two requests for ten props in a row both run, and the cheat drill's "
    "budget arm must FAIL.")
