// coop/config/config_registry_rows_dev_disc.inc -- the [dev] rows of the disc drills (the full disc's ceiling, the
// module row's intent). Included by config_registry_rows_dev.inc after the cremator rows. The entry forms are
// config_registry_rows.inc's.

CFG_FLAG(disc_ceiling_32k, "disc_ceiling_32k", "dev", false, "VOTVCOOP_DISC_CEILING_32K",
    "Developer red arm of the full-disc drill: never set. A disc's record and slot ceiling is 32 KB again, as it was "
    "before the transport's own maximum was measured to carry a disc of 32 writes, so a full disc is refused at the "
    "peer that holds it and does not cross.")
CFG_FLAG(module_intent_off, "module_intent_off", "dev", false, "VOTVCOOP_MODULE_INTENT_OFF",
    "Developer red arm of the module-row drill: never set. This client's press on an upgrade panel's module row runs "
    "on this machine alone, as it did before the press was an intent: the host never takes it, and its next balance "
    "gives the charge back.")
CFG_ENUM(disc_eject_drill, "disc_eject_drill", "dev", "off", "off|host|client|zip|nofix",
    "VOTVCOOP_DISC_EJECT_DRILL",
    "Drill (BOTH peers): a disc edited in the PC and ejected. The host seeds a floppy with three marked rows "
    "and 20 writes; the acting peer (the host; the client for `client`) inserts it, drops its first row, adds "
    "one, spends a write and ejects; the disc must then hold the edited rows and 19 writes on both peers. "
    "`zip`: the same with a zip drive. `nofix`: the host acts with the game fix off, the control arm, which "
    "must FAIL. One `[DISC-EJECT-DRILL] [H|C] PASS` or `FAIL` line from the acting peer. Leaves the disc in "
    "the world: a throwaway save only.")
CFG_ENUM(food_sleep_drill, "food_sleep_drill", "dev", "off", "off|on|nofix", "VOTVCOOP_FOOD_SLEEP_DRILL",
    "Drill (each peer on its own player): sleep set to 40, then the game's addFood with sleep 20 at coffee "
    "tolerance 0 (60 wanted) and 0.5 (50 wanted), then a refused eat at full hunger (40 wanted); the stats are "
    "put back. `nofix`: the same with the game fix off, the control arm, which must FAIL. One "
    "`[FOOD-SLEEP-DRILL] [H|C] PASS` or `FAIL` line per peer.")
