// coop/config/config_registry_rows_dev_pile.inc -- the [dev] row of the pile deferral drill.
// Included by config_registry_rows_dev.inc after the coffee rows, before the ATV rows, as _dev_octa.inc is. The entry
// forms are config_registry_rows.inc's.

CFG_ENUM(pile_defer_drill, "pile_defer_drill", "dev", "off", "off|check|giveup", "VOTVCOOP_PILE_DEFER_DRILL",
    "Drill (BOTH peers, the same value; run it on a fresh host world that rehosts, with one client): the joiner counts "
    "every pile whose bind was deferred to the quiescence sweep, reads bind_wait's tallies and, 20 s after its "
    "snapshot completes, sends the host the pose of every trash element it has bound. The host compares each with its "
    "own (a pile at rest within 2 cm and 2 deg; a moving one is listed, not judged) and names any pile the joiner "
    "lacks. check: nothing is steered; the verdict is each peer's DONE line or a FAIL line, both tagged [PILE-DEFER]. "
    "giveup: with pile_late_load=1 the joiner's kept-aside clump is destroyed once its row defers, to stand in for a "
    "native that never loaded; the sweep must give up on it, its held converts must replay unbound, and the pile must "
    "materialise at the host's pose.")

CFG_INT(pile_late_load, "pile_late_load", "dev", 0, 0, 1, "VOTVCOOP_PILE_LATE_LOAD",
    "Drill seam (the joining client, with pile_defer_drill=check|giveup on both peers): 1 keeps the first save-loaded "
    "clump the join's load catch holds 200 cm aside from its saved pose, an artificial late load, so its row defers "
    "to the sweep. Once it has deferred, check puts the clump back at its saved pose (the sweep must bind it) and "
    "giveup destroys it as a native that never loaded (the sweep must give up). One clump per session; nothing "
    "happens with pile_defer_drill=off. 0 = the join's natives load where the save put them.")

CFG_INT(trash_hold_red, "trash_hold_red", "dev", 0, 0, 1, "VOTVCOOP_TRASH_HOLD_RED",
    "Drill red (the joining client, with pile_defer_drill=check on both peers): 1 turns the join's load catch off, so "
    "a save-loaded clump in flight is no longer held at its saved pose and settles off its key again; the drill must "
    "then FAIL on orphans. 0 = the catch holds as designed.")
