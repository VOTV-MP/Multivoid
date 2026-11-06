// coop/config/config_registry_rows_dev_disc.inc -- the [dev] rows of the disc drills (the full disc's ceiling). Included by config_registry_rows_dev.inc after the cremator rows. The entry forms are
// config_registry_rows.inc's.

CFG_FLAG(disc_ceiling_32k, "disc_ceiling_32k", "dev", false, "VOTVCOOP_DISC_CEILING_32K",
    "Developer red arm of the full-disc drill: never set. A disc's record and slot ceiling is 32 KB again, as it was "
    "before the transport's own maximum was measured to carry a disc of 32 writes, so a full disc is refused at the "
    "peer that holds it and does not cross.")
