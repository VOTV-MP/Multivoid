// coop/config/config_registry_rows_dev_orb.inc -- the [dev] rows of the black orb's drill (coop/creatures/orb_authority):
// its arm and its red. Included by config_registry_rows_dev.inc after the UFO rows, so each row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(orb_begin_send_off, "orb_begin_send_off", "dev", false, "VOTVCOOP_ORB_BEGIN_SEND_OFF",
    "Developer red arm of the orb drill: never set; the host. The host sends no Begin for an orb's encounter, so a "
    "client's copy runs none and the drill must FAIL when the host's destroy of that copy arrives.")
CFG_ENUM(orb_drill, "orb_drill", "dev", "off", "off|1", "VOTVCOOP_ORB_DRILL",
    "Drill, the black orb's encounter; set on BOTH peers, and VOTVCOOP_CAN_DIR on both. 1 -- the host mints an orb on "
    "the client's puppet twice, after the client has walked 30 m from the host (the director): with the view away from it the host's verdict must be spared "
    "and the client's copy must have run the host's Begin by the time the host's destroy reaches it; with the view "
    "toward it the verdict must be caught and the client must answer the host's Death: 'rig: VERDICT orb "
    "PASS|FAIL|INVALID' and 'orb-drill: DONE on host'. Red arm: orb_begin_send_off on the host. It MUTATES the world "
    "(orbs are minted); the caught client is revived.")
