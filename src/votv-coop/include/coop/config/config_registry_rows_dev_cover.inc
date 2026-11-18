// coop/config/config_registry_rows_dev_cover.inc -- the [dev] rows of the cover drill (the signal desk's back plates and
// a transformer station's door): its arm and its four red arms. Included by config_registry_rows_dev.inc after the
// cargo lift rows. The entry forms are config_registry_rows.inc's.

CFG_ENUM(cover_drill, "cover_drill", "dev", "off", "off|run|measure", "VOTVCOOP_COVER_DRILL",
    "Drill (BOTH peers): the desk's back plates and a station's door are the host's; set on BOTH peers, and "
    "VOTVCOOP_CAN_DIR on both. run -- the host changes a plate and the door before the client's world is up (the "
    "join), then the client walks to the desk with the director and unbolts a plate, asks the same unbolt again (the "
    "host answers Already), takes the plate off, sets it back, asks a put-back nobody carried (refused), walks to the "
    "station, swings its door and asks a desk verb from out of reach (refused): each peer compares its plates and door "
    "with the other's, and the host's `rig: VERDICT cover PASS` ends it; the first arm that fails says FAIL and no later "
    "arm runs. measure -- the same run, with both peers logging every updPanels call, the desk's overlaps, the door's "
    "end and openDoor, the unbolt's sound timer and every station's index (`[COVER-MEASURE]`). It MUTATES the world. Run "
    "it on a fresh host world.")
CFG_FLAG(cover_judge_off, "cover_judge_off", "dev", false, "VOTVCOOP_COVER_JUDGE_OFF",
    "Developer red arm of the cover drill: never set, on the host. A client's desk ask is applied without the reach "
    "judge, so the out-of-reach arm's unbolt lands and the arm says FAIL.")
CFG_FLAG(cover_wanted_off, "cover_wanted_off", "dev", false, "VOTVCOOP_COVER_WANTED_OFF",
    "Developer red arm of the cover drill: never set, on the host. An unbolt ask toggles the plate whatever state it "
    "carries, so the repeated unbolt bolts the plate again and the wanted-state arm says FAIL.")
CFG_FLAG(cover_hold_judge_off, "cover_hold_judge_off", "dev", false, "VOTVCOOP_COVER_HOLD_JUDGE_OFF",
    "Developer red arm of the cover drill: never set, on the host. A put-back is applied without the hold, the lid "
    "touch and the fresh-prop tests, so a plate prop nobody carried back is eaten and the put-back arm says FAIL.")
CFG_FLAG(cover_announce_off, "cover_announce_off", "dev", false, "VOTVCOOP_COVER_ANNOUNCE_OFF",
    "Developer red arm of the cover drill: never set, on the host. The host does not send the desk's plates when the "
    "game updates them, so an unbolt it judged never reaches the client and the unbolt arm says FAIL.")
