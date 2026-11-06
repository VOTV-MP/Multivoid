// coop/config/config_registry_rows_dev_kerfur.inc -- the [dev] rows of the kerfur drills (the menu turn-on, the
// disc-carrying conversion, the Kerfus's settle and throw, the Omega's serve), moved in their order. Included by
// config_registry_rows_dev.inc where they stood, so each row keeps its place in every expansion of the list. The
// entry forms are config_registry_rows.inc's.

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
    "commands idle, the host runs the kill verb, and the murderous Omega must go for the host. The drill ends "
    "on 'rig: VERDICT kerfur-serve PASS|FAIL|INVALID' (INVALID is a step that could not run).")
CFG_FLAG(kerfur_command_gate_off, "kerfur_command_gate_off", "dev", false, "VOTVCOOP_KERFUR_COMMAND_GATE_OFF",
    "Developer red arm: never set. On a client the seat on the Omega's actionName runs every action, so a "
    "relayed radial command (follow, idle, patrol, fix servers, get reports, fix transformers) runs on the client's "
    "own copy and is not asked of the host: no 'taken at the seat' line, no command sent, and the client's disc is "
    "destroyed by its own get_reports. The host's side is unchanged. The serve drill's leg D must then end FAIL, "
    "its own copy having run the verb.")
