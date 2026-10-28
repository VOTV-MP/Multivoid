// coop/config/config_registry_rows_dev_pr64.inc -- the [dev] rows of the pr64 drill: a press of E on what the host owns,
// measured on both peers, and the two reds that put back a behaviour the PR removed. Included by
// config_registry_rows_dev.inc after the container rows. The entry forms are config_registry_rows.inc's.

CFG_ENUM(pr64_drill, "pr64_drill", "dev", "off", "off|coin|lid", "VOTVCOOP_PR64_DRILL",
    "Drill (BOTH peers): a press of E on what the host owns, judged by each peer's own line. coin -- the host "
    "spawns a coin gun and a prop at a floor point a nav route reaches from the host and from the client's body, "
    "mints one coin through the gun's own sell verb and names its element id; the client walks to it with the "
    "director, aims at it and presses E through the game window, as a player does. The client passes when it "
    "credited itself nothing before the host's broadcast (a watch of lib_C::addPoints counts its own credits), the "
    "coin is gone and its balance equals the host's; the host passes when its balance rose by the coin's points "
    "and the coin is gone. lid -- the host spawns a drive box in front of its own player with a drive in slot 0, "
    "holds E on it until it opens, then holds E on slot 0 until the drive comes out; the host says the box's "
    "opened bit and slot 0, and the client passes when its own copy reads the same pair within 2 s. The host "
    "marks each change in its log so the record publishes between marks can be counted. Lines tagged "
    "[PR64-DRILL]; the host's DONE line ends it. It MUTATES the world.")

CFG_INT(pr64_record_window_red_ms, "pr64_record_window_red_ms", "dev", 0, 0, 60000, "VOTVCOOP_PR64_RECORD_WINDOW_RED_MS",
    "Drill red (BOTH peers, with pr64_drill=lid): when above 0, a drive box's record refresh drops a publish for a "
    "box whose last publish is younger than this many milliseconds, the leading-edge window the refresh no longer "
    "has. The drill's second change falls inside it, so the far box must end without it and the client must print "
    "its FAIL line. 0 = every change publishes, as designed. Read only when pr64_drill is set.")

CFG_FLAG(pr64_coin_local_credit_red, "pr64_coin_local_credit_red", "dev", false, "VOTVCOOP_PR64_COIN_LOCAL_CREDIT_RED",
    "Drill red (the client, with pr64_drill=coin): the E-press verdict on a host-owned coin forwards the collect "
    "and then lets the native credit run, the behaviour before the press was cancelled. The client credits itself "
    "at the press, and its FAIL line must say so. Off = the press is cancelled after the forward, as designed. "
    "Read only when pr64_drill is set.")
