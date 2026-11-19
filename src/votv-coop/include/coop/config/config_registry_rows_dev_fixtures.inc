// coop/config/config_registry_rows_dev_fixtures.inc -- the [dev] rows of the drills for a fixture a player works
// on. Included by config_registry_rows_dev.inc, so its rows join every expansion of the list. The entry forms are config_registry_rows.inc's.
CFG_ENUM(oven_progress_drill, "oven_progress_drill", "dev", "off", "off|host|client|join|repair|reach|order|fixearly|again|burst",
    "VOTVCOOP_OVEN_PROGRESS_DRILL",
    "Drill (BOTH peers): the oven's screws and lid. host and client: the acting peer sets a screw's "
    "bool on its own repair screen, then the other three, then the lid's; the watching peer says its "
    "screen's mask whenever it changes and each step must stand there before the next; the client is "
    "walked to the oven's switch box first. join: the host's screen takes all five before anyone joins "
    "and the joiner judges its own screen when the host's mask reaches it. repair: the walked client "
    "writes the screws and the lid on its screen (not its click handlers), then calls the oven's fix(); the host's judge grants it "
    "and every peer must read the oven fixed, the client's own fix() never having run. reach, order and "
    "fixearly: the host's judge refuses a raw screw bit from out of reach (run with 2 clients: the other "
    "client must never see the refused bit), a raw lid bit with no screws, and a raw repair with no lid; "
    "each is decided on the host at the event that settles it (its own refusal counter moving, or the refused "
    "bit reaching the host's record or fixing its oven, a FAIL). again: the repair is asked twice and the second "
    "is answered as already fixed. burst: twelve raw screw messages at once, more than the host's queue holds. "
    "One `[OVEN-DRILL] [H|C] PASS` or `FAIL` "
    "line per judge, each decided at the event that settles it; a wait that never ends says "
    "`INVALID`. An oven the save holds repaired ends INVALID by name.")
CFG_FLAG(oven_judge_off, "oven_judge_off", "dev", false, "VOTVCOOP_OVEN_JUDGE_OFF",
    "Developer red arm of the oven drill's reach, order and fixearly arms: never set. The host's judge skips "
    "the reach and order checks, so the raw bit reaches the host's record (or the raw repair fixes its oven) "
    "and the arm must say FAIL at that event.")
CFG_FLAG(oven_repair_hold_off, "oven_repair_hold_off", "dev", false, "VOTVCOOP_OVEN_REPAIR_HOLD_OFF",
    "Developer red arm of the oven drill's repair arm: never set. A client runs its own fix() instead of "
    "holding it for the host's grant, so its oven reads fixed before the grant and the arm must say FAIL.")
CFG_FLAG(oven_merge_off, "oven_merge_off", "dev", false, "VOTVCOOP_OVEN_MERGE_OFF",
    "Developer red arm of the oven drill: never set. This peer counts a repair-step mask that reaches it "
    "with a bit its screen lacks and does not raise the bit, so the drill's judge on this peer must FAIL at "
    "that arrival.")
CFG_ENUM(pried_drill, "pried_drill", "dev", "off", "off|host|tv|client|far|twice|join|midjoin",
    "VOTVCOOP_PRIED_DRILL",
    "Drill (BOTH peers): a crowbar's pry of a prop with no wall-attach component, judged from the host. The host "
    "spawns the fixture on open ground beside the client's puppet and the client finds its copy. host and tv: the host "
    "pries a radiator or a security TV; each client's joints the engine's BreakConstraint broke must equal the host's, "
    "and the copies must rest within 1 cm and 1 degree after the host's last drive End. client: the client pries a "
    "plank by the game's own activatePrying and End; the host's grant is the event, then the rest gap. far: a raw ask "
    "for a plank out of the sender's reach; the host's refusal is the event. twice: the host's second crowbarOpen of "
    "a pried plank is cancelled and its body ran once. join: the client rejoins after the host pried a radiator, and "
    "its joints must equal the host's. midjoin: the host pries while the client is joining. Run join and midjoin with "
    "the rig's --rejoin, --rejoin-marker 'pried-drill: REJOIN-NOW' and --rejoin-env VOTVCOOP_PRIED_LIFE=2. One "
    "`rig: VERDICT pry-<arm> PASS|FAIL|INVALID` line, decided at the event that settles it; a wait that never ends "
    "says INVALID. Run it on a fresh host world.")
CFG_FLAG(pry_judge_off, "pry_judge_off", "dev", false, "VOTVCOOP_PRY_JUDGE_OFF",
    "Developer red arm of the pried drill's far arm: never set. The host's judge skips the reach check, so the "
    "out-of-reach ask is granted and the arm must say FAIL.")
CFG_FLAG(pry_ask_off, "pry_ask_off", "dev", false, "VOTVCOOP_PRY_ASK_OFF",
    "Developer red arm of the pried drill's client arm: never set. A client runs its own crowbarOpen instead of "
    "asking the host, so the plank frees on the client alone and the arm must say FAIL at the pry's end.")
CFG_FLAG(pry_coast_off, "pry_coast_off", "dev", false, "VOTVCOOP_PRY_COAST_OFF",
    "Developer red arm of the pried drill's host arm: never set. The host hands no freed body to the driven-prop "
    "stream, so each peer simulates the fall and the copies' rest gap must say FAIL.")
CFG_FLAG(pry_row_off, "pry_row_off", "dev", false, "VOTVCOOP_PRY_ROW_OFF",
    "Developer red arm of the pried drill's join arm: never set. The host's prop row carries no pry, so the joiner's "
    "joints must differ from the host's and the arm must say FAIL.")
CFG_FLAG(pry_again_off, "pry_again_off", "dev", false, "VOTVCOOP_PRY_AGAIN_OFF",
    "Developer red arm of the pried drill's twice arm: never set. The host does not cancel a second crowbarOpen of "
    "a pried prop, so its body runs twice and the arm must say FAIL.")
