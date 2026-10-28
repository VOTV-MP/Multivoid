// coop/config/config_registry_rows_dev_desk.inc -- the [dev] rows of the signal workstation's drills (the desk's verbs
// and crossings, the drives, the towers, the ping, the download, the laptop and the power grid). Included by
// config_registry_rows_dev.inc at the place the rows stood, as _dev_swinger.inc is, so each row keeps its place in
// every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_STRING(desk_verb_drill, "desk_verb_drill", "dev", "", "VOTVCOOP_DESK_VERB_DRILL", false,
    "Drill (BOTH peers): the main desk's save family pressed by the client through its own input. run: the host "
    "arms a caught signal formed from a sky signal's object, downloaded and detected, and the client, walked to the "
    "desk, presses SAVE; then DELETE on a second one, the deck's send on the saved row, the deck's drive button "
    "both ways and the refiner's upload, then the refiner's start, its stop, a start the host completes and one at "
    "level 2 whose completion is the last level's. Both peers count the game's own entries into the functions those "
    "branches call; the host says it entered them, the client that it entered none, that its refiner never latched, "
    "that SAVE's and each completion's gloss and the last level's processed point reached its profile and the row "
    "its photo. join: the host decodes on its refiner and saves a row as the client joins; the client finds the row "
    "with its photo, and its restore of the decode refused, mirroring the host's. Lines tagged [DESK-VERB-DRILL]; "
    "'client DONE PASS|FAIL' ends it; ABANDONED is a step that could not run.")
CFG_STRING(desk_crossing_drill, "desk_crossing_drill", "dev", "", "VOTVCOOP_DESK_CROSSING_DRILL", false,
    "Drill (BOTH peers): the main desk's detection needle crosses on the host alone. run: two legs, planeteater's "
    "caught signal (its object renderer spawns the signal object fullyProcessed lives on) and looker_behind's; the "
    "host holds its needle just below 1 for three of its play cycles, then lets its step cross. The client says its "
    "own step never crossed, that it ran its desk's path after the step once, and on looker_behind that its autoSave "
    "body never ran while the host's saved the one row; at the crossing canDL holds on both, and on planeteater "
    "fullyProcessed. join: the host crosses on planeteater before the client's world is ready; the client relays no "
    "catch of its own, reads the host's needle at 1, and finds canDL and fullyProcessed. Lines tagged "
    "[DESK-CROSSING-DRILL]; 'client DONE PASS|FAIL' ends it; ABANDONED is a step that could not run.")
CFG_STRING(drive_drill, "drive_drill", "dev", "", "VOTVCOOP_DRIVE_DRILL", false,
    "Drill (BOTH peers): the host gives a drive a row; a client's deviation on its copy must be put back at once, "
    "its eraser press must wipe the drive on the host and show here, a drive it spawns, takes out or throws must "
    "reach the host with its row, and its forged row on a drive it did not bring must be refused. "
    "run -- a drive the host spawns once a client is in its world; join -- a drive of the host's world, written "
    "while a joiner loads it; obs -- run's steps, begun once two clients stand in the world, the second (the rig's "
    "observer) only watching the drive lane. Lines tagged [DRIVE-DRILL]; the host's DONE line ends it.")
CFG_STRING(tower_drill, "tower_drill", "dev", "", "VOTVCOOP_TOWER_DRILL", false,
    "Drill (BOTH peers): the coordinate towers. run: once the client stands in its world, the host breaks the "
    "drill's tower (the one nearest the alpha base) on its own dice and opens its panel; the client presses a "
    "puzzle button and pulls a blown fuse from where it stands, both refused out of reach and the pulled fuse "
    "reaped; then the host's player pulls a fuse, inserts one, presses a button, pulls the lever on the unsolved "
    "puzzle, solves it and pulls the lever again, and the client reads each change applied as the tower's own. "
    "join: the host breaks the tower as it hosts, and the joiner's tower reads the host's without a roll of its "
    "own. red, joinred: the controls, a client whose gates stand open and who applies no rows, as before the "
    "lane. The radio tower (every client arm walks to the tower with the director, then to its ladder's foot, "
    "mounts the ladder through its own use and climbs to the top, where the parts are within a use's reach; a "
    "refused mount or a short climb ends it ABANDONED): radioreach -- prints how far the climb ended from each "
    "part (a measurement, no verdict); radio -- the host scrambles its tower until it reads broken, the client "
    "reads the break, solves the puzzle with presses through the tower's own use and pulls the confirm lever, and "
    "the verdict is whether the host's tower and the client's copy both read whole; radiored -- radio with the "
    "client's gates open, which fails as the lane's first build did; radioreset -- the client pulls the reset "
    "lever: the host's cells must re-roll, this copy show them, and this copy's own roll be refused; radiojoin -- "
    "the host breaks its tower at its own world-ready, before the client joins, and the client's first row for the "
    "tower must already read broken; it then repairs as radio does; radiojoinred -- radiojoin with the client's "
    "gates open, which fails as the break never reaches its copy; radioresetred -- radioreset with the gates open, "
    "which fails as the host's cells do not change (a re-roll that repeats the host's cells is pulled again, three "
    "times at most, then INCONCLUSIVE); radiofuse -- the host blows a fuse of its tower and keeps it whole, the "
    "client pulls that fuse, the host hands it a good one, the client inserts that into the emptied slot (needs "
    "difficulty 2 or more), and the host's slot must read good; radiofusered -- radiofuse with the gates open, which fails "
    "naming the host's slot left unchanged. The repair arms want the game rule enableLightsOut on, or the tower "
    "has no puzzle to press (radio then prints lightsout=0). Lines tagged [TOWER-DRILL]; the client's DONE line "
    "ends it.")
CFG_STRING(desk_ping_drill, "desk_ping_drill", "dev", "", "VOTVCOOP_DESK_PING_DRILL", false,
    "Drill (BOTH peers): a client's ping is rolled on the host. run: the client walks to the main desk, enters the "
    "coordinates screen, commits a triangle and presses its coord_ping key on the game window; legs: miss (a clear "
    "sky), catch (the host's fixture signal under the triangle), busy (a second verdict right behind the catch's). "
    "The client says its gatherSignal was entered and never ran, the host's failure line reached its log once, the "
    "catch is on its profile and desk, and the busy verdict came back refused; the host that it rolled two, caught "
    "the second without counting it as its own, relayed it as the client's, and refused the third. Every peer that "
    "does not ping (the host, and a second client under --observer) also reads its own desk: the first edge line's "
    "alpha must pass 0.5 at stage 0 and the first inner line's at stage 1 with coord_isPing clear, and all four "
    "values must read 0 after the scan's ending; a second client says observer PASS|FAIL per scan, and observer DONE "
    "once the first client, which leaves the desk before its DONE, has left it. observed: the same, but the client "
    "starts only once the observer in slot 2 is ready (rig --observer), and the observer must judge both scans, the "
    "miss and the catch; under run one judged scan passes. Lines tagged [DESK-PING-DRILL]; 'client DONE' ('observer "
    "DONE' under --observer) ends it; ABANDONED is a step that could not run.")
CFG_STRING(download_drill, "download_drill", "dev", "", "VOTVCOOP_DOWNLOAD_DRILL", false,
    "Drill (BOTH peers): the host arms and resets its download five times; the client must replay each once with the "
    "host's values (formDownload's body once an arm, one reset line a reset, the reset clean) and have its own "
    "arm and reset refused. run -- once a client is in its world; join (with --rejoin and --rejoin-marker "
    "'[DOWNLOAD-DRILL] client LIFE 1 DONE') -- an arm while the client's first life is in, then in the rejoiner's "
    "window a reset, a second catch and its arm, which it must end armed on. Lines tagged [DOWNLOAD-DRILL]; the "
    "client's DONE line ends it.")
CFG_STRING(laptop_drill, "laptop_drill", "dev", "", "VOTVCOOP_LAPTOP_DRILL", false,
    "Drill (BOTH peers): the laptop's disc slot is the host's and its file edits land on the disc they were made on. "
    "run: the client inserts a disc and edits its files before the host answered the claim, ejects it, inserts a "
    "second; the host ejects that one ahead of the client's next edit, then a batch and a canonical of the ejected "
    "disc, each on its old generation, must land nowhere. join: the host inserts a disc and adds a buffer row while "
    "a joiner loads, and the joiner's laptop must hold both. Lines tagged [LAPTOP-DRILL]; the client's DONE line "
    "ends it.")
CFG_STRING(grid_drill, "grid_drill", "dev", "", "VOTVCOOP_GRID_DRILL", false,
    "Drill (BOTH peers): the power grid, arm by arm. run: the client presses the panel's light lever, the host "
    "breaks the drill's generator (the one nearest the panel), the client's Activate press from where it stands is "
    "refused beyond reach, and the host's own repair reaches the client. puzzle: the host's puzzle and inputs reach "
    "the client, whose own input from out of reach rolls back. puzzlesolve: the client, walked beside the generator, "
    "solves its puzzle and presses Activate, which the host takes; puzzleflood: from there it sends 40 inputs at "
    "once, which the host must take within its rate for inputs, its rows coalesced. upgrade: from there an "
    "install op with no "
    "insert behind it is refused, nothing refunded, the client's install is taken, and a refused one is refunded. "
    "upgraderepair: the client installs one upgrade, the host breaks the generator, the client solves its puzzle and "
    "presses Activate, and 20 s after the repair both peers must still read the upgrade and its mesh; upgraderepairred "
    "installs nothing and must fail on level 0; upgraderepairlossred has the host take the upgrade off its own "
    "generator after the repair, and both peers' verdicts must fail. "
    "join, lockout, lockjoin: the blackout at a join, the desk virus's lockout. red, puzzlered, upgradered: the "
    "controls (the rows applied raw, no puzzle written, installs judged unpaired). Lines tagged [GRID-DRILL]; the "
    "client's DONE line ends it.")
