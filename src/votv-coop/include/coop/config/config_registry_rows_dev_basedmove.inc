// coop/config/config_registry_rows_dev_basedmove.inc -- the [dev] rows of the based-movement drill: its arms and its red.
// Included by config_registry_rows_dev.inc after the flood rows. The entry forms are config_registry_rows.inc's.

CFG_ENUM(basedmove_drill, "basedmove_drill", "dev", "off", "off|ride|hostride|bad_base_xy|bad_base_vol|baseend", "VOTVCOOP_BASEDMOVE_DRILL",
    "Drill (BOTH peers, default off; mp.py smoke --host-fresh on a fresh game): a pose names the mover its player stands on "
    "(the base's cargo lift), and the receiver rides the puppet on it. ride -- the client is walked onto the lift's platform by "
    "the director, the host presses the lift and, every frame, logs the platform, the gap its own block test reads, the "
    "puppet's offset from where its pose puts it, its feet against the slab and what it is attached to: PASS when the lift "
    "reaches its stop, FAIL at the first reversal. hostride -- the HOST rides and the client watches its puppet of the host "
    "against its own copy of the platform: PASS at the lift's stop when the puppet's feet stayed within 20 cm of the slab on "
    "every frame, FAIL at the first frame past it. bad_base_xy and bad_base_vol -- the client forges its claim (a base offset "
    "200 cm off its absolute pose; an absolute height past the lift's travel): PASS when the host sets each claim aside and its "
    "puppet follows the absolute pose, FAIL if the puppet rides the forged claim. baseend -- the client rides; mid-travel, once "
    "the host's puppet is attached, the host destroys its own copy of the platform actor (K2_DestroyActor, said in its log): PASS "
    "at the platform's end of play and the next frame when the puppet still exists, has no parent and stands within 30 cm of "
    "where it stood; FAIL at the first of: destroyed with its base, still parented, moved. Verdicts: 'rig: VERDICT "
    "basedmove-<arm> PASS|FAIL|INVALID'. Red: basedmove_base_off (ride, hostride); baseend has none, it proves what the engine does.")
CFG_FLAG(basedmove_base_off, "basedmove_base_off", "dev", false, "VOTVCOOP_BASEDMOVE_BASE_OFF",
    "Developer red arm of the based-movement drill (the rider's peer): never set. The pose names no base, so the receiver "
    "places the rider by its absolute pose alone, as before the base existed; ride must say FAIL at the lift's reversal and "
    "hostride at the first frame its puppet's feet leave the slab by more than 20 cm.")
