// coop/config/config_registry_rows_dev_grime.inc -- the [dev] rows of the grime lane (coop/interactables/grime_sync,
// coop/world/spill_sync): its log, the rehost churn's red arm, the clean drill's arms and reds, and the judge's reds.
// Included by config_registry_rows_dev.inc at the place the grime rows stood, so each row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(grime_log, "grime_log", "dev", false, "VOTVCOOP_GRIME_LOG",
    "Diagnostic logging for grime/cleaning sync: each fall sent, each value applied, and every decal an index pass finds, one line each.")
CFG_FLAG(grime_replay_watch_off, "grime_replay_watch_off", "dev", false, "VOTVCOOP_GRIME_REPLAY_WATCH_OFF",
    "Developer red arm of the rehost churn (VOTVCOOP_RUN_RELOAD_CHURN=1 VOTVCOOP_RELOAD_HOST=1 VOTVCOOP_RELOAD_CYCLES=2): "
    "never set; the host. The grime lane does not forget its index as the save replay begins, so the replay's deletes of "
    "the level's own decals read as wipes to zero and the save's decals at those places are lowered. That needs the session "
    "live as the replay runs, which a rehost through the menu does by timing alone: the arm counts only the cycles "
    "whose replay the lane saw, so the red says FAIL on a run that had one, and INVALID when none did.")
CFG_ENUM(clean_drill, "clean_drill", "dev", "off", "off|clean|leaves|leaves_join", "VOTVCOOP_CLEAN_DRILL",
    "Drill, the grime lane on THREE peers (the host and two clients, mp.py --clients 2); set on ALL, with VOTVCOOP_CAN_DIR "
    "on all, on the rig's save (it holds the bucket the targets are picked around). clean -- the first client (VOTVCOOP_CLEAN_ACTOR=host: the host) cleans away, in the "
    "rain's call form (a client with a sponge it spawns and holds, walking to each decal, strokes it until the host's word ends it), one decal of each grime kind near the save's bucket (VOTVCOOP_CLEAN_BUCKET names its key); every peer says whether its copy ended, the actor's last clean (a sentinel decal's fall, behind its "
    "ends on the ordered wire) being the moment each peer decides: 'rig: VERDICT grime-clean PASS|FAIL|INVALID', FAIL "
    "naming the decals that stood and the peer; every peer dumps the grime it holds before and after ('[CLN] all'). leaves -- every peer runs the autumn leaf spawn the wind's timer runs; "
    "every peer must hold the host's fallen leaves at the host's places and none of its own: 'rig: VERDICT grime-leaves "
    "PASS|FAIL|INVALID'. leaves_join -- the late-join answer, TWO peers (the host and the first client, mp.py --clients 1 "
    "--rejoin): the host grows its fallen leaves, the client holds them, then leaves and joins again (its second life run "
    "with leaves_rejoin), and must hold the host's leaves at the host's places: 'rig: VERDICT grime-leaves-join "
    "PASS|FAIL|INVALID' said by the host. Red arms: grime_gone_off (clean) and leaves_lane_off (leaves, leaves_join). "
    "It MUTATES the world.")
CFG_FLAG(leaves_rejoin, "leaves_rejoin", "dev", false, "VOTVCOOP_LEAVES_REJOIN",
    "Drill (leaves_join), the REJOINED client's second life only (mp.py --rejoin-env): it needs no barrier with the other "
    "peer, only the host's fresh mark of where its fallen leaves stand, and says it holds them.")
CFG_FLAG(grime_gone_off, "grime_gone_off", "dev", false, "VOTVCOOP_GRIME_GONE_OFF",
    "Developer red arm of the clean drill (clean): never set; on every peer. A decal's end goes out as a fall to zero, as "
    "it did before the ended mark: every other peer's copy reads zero and stays standing, so the drill must say 'rig: "
    "VERDICT grime-clean FAIL' naming the decals that stood.")
CFG_FLAG(leaves_lane_off, "leaves_lane_off", "dev", false, "VOTVCOOP_LEAVES_LANE_OFF",
    "Developer red arm of the clean drill (leaves): never set; on every peer. The spill lane does not take the fallen "
    "leaves an autumn leaf spawner makes, so the host's are sent to no one and each client keeps its own: the drill must "
    "say 'rig: VERDICT grime-leaves FAIL'.")
CFG_FLAG(grime_judge_off, "grime_judge_off", "dev", false, "VOTVCOOP_GRIME_JUDGE_OFF",
    "Developer red arm of the grime judge's drill (the hostile_drill grime_* arms): never set; on the host. The host applies a "
    "client's grime message as it came, as before the judge: a fall from beyond reach, with nothing in hand, to zero from a "
    "full decal, an ended mark and a refused fall in a joiner's snapshot all stand, so far, notool, gone, fast and joinleak "
    "must FAIL at the host's value.")
CFG_FLAG(grime_rain_open, "grime_rain_open", "dev", false, "VOTVCOOP_GRIME_RAIN_OPEN",
    "Developer red arm of the grime judge's drill (grime_dry): never set; on the host. The rain route of the judge skips its "
    "checks (the host's own rain, the decal's rain test, the roof and the radius), so an empty hand's fall on a dry host is "
    "granted and dry must FAIL.")
CFG_FLAG(grime_hold_off, "grime_hold_off", "dev", false, "VOTVCOOP_GRIME_HOLD_OFF",
    "Developer red arm of the grime judge's drill (grime_last): never set; on the client. The client's clean destroys "
    "its decal as the game wrote it, so the decal is gone before the host's word and the arm must FAIL at the client's "
    "decal gone ahead of the host's end.")
