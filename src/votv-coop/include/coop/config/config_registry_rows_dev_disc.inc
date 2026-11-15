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
CFG_STRING(event_fire_drill, "event_fire_drill", "dev", "", "VOTVCOOP_EVENT_FIRE_DRILL", false,
    "Drill (BOTH peers): `<name>[:<seconds>]`. Once a client stands in its world the host fires the named "
    "event of the dev event table (the F1 menu's list) through the menu's own dispatch, then the run stays up "
    "for the seconds given (60 by default) so both logs record what each lane did with it. Lines tagged "
    "[EVENT-FIRE-DRILL]; the host's DONE line ends it. No verdict of its own.")
CFG_STRING(host_place, "host_place", "dev", "", "VOTVCOOP_HOST_PLACE", false,
    "Drill aid (host): `x,y,z`. The host's own player is put there once its world is up, before anyone joins, so "
    "a drill whose client joins at the host and walks starts from a known spot. One [HOST-PLACE] line.")
CFG_STRING(event_fire_walk, "event_fire_walk", "dev", "", "VOTVCOOP_EVENT_FIRE_WALK", false,
    "Drill aid (host), with event_fire_drill: `x,y,z`. Five seconds after the fire the host's player is put there, "
    "for an event whose actor waits for a player to walk into it (the Rozital hole). One [EVENT-FIRE-DRILL] line.")
