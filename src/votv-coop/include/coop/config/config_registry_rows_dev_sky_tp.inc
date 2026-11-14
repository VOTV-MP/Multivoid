// coop/config/config_registry_rows_dev_sky_tp.inc -- the [dev] rows of the earth teleport's sky drill (coop/world/sky_sync):
// its arm and its red. Included by config_registry_rows_dev.inc after the orb rows, so each row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(sky_flag_apply_off, "sky_flag_apply_off", "dev", false, "VOTVCOOP_SKY_FLAG_APPLY_OFF",
    "Developer red arm of the sky drill: never set; the client. A client reads the host's black hole flag in a SkyState "
    "and never applies it, so the drill must FAIL where the first state carrying the hole is counted.")
CFG_ENUM(sky_tp_drill, "sky_tp_drill", "dev", "off", "off|1|join", "VOTVCOOP_SKY_TP_DRILL",
    "Drill, the earth teleport's black hole; set on BOTH peers, and VOTVCOOP_CAN_DIR on both. 1 -- the host runs the "
    "sky's tp: the client's sky must show the hole at the first SkyState that carries it and lose it at the first one "
    "after its end (about 90 s in all): 'rig: VERDICT sky-tp PASS|FAIL|INVALID' and 'sky-tp-drill: DONE on host'. "
    "join -- with the rig's --rejoin: the host holds its hole, the client leaves, and the rejoined client (second life: "
    "VOTVCOOP_SKY_TP_DRILL_REJOIN=1) must have its first SkyState carry the hole and its sky show it: 'rig: VERDICT "
    "sky-tp-join PASS|FAIL|INVALID'. Red arm: sky_flag_apply_off on the client. It MUTATES the sky.")
