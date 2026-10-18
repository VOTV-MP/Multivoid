// coop/config/config_registry_rows_dev_pi.inc -- the [dev] rows of the puppet-identity arc (the PI-1 and PI-2
// selftests and the PI-3 drill), included by config_registry_rows_dev.inc where they stood, so each keeps its
// place in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(pi1_selftest, "pi1_selftest", "dev", false, "VOTVCOOP_PI1_SELFTEST",
    "Autonomous selftest (BOTH peers): the script gate's scope mark, a native post hook's bytecode call "
    "site, a const reference into a native pre hook and a caller's local edited in place, each one "
    "[PI-1] PASS or FAIL line, then [PI-1] DONE. Calls two read-only library functions and turns the "
    "local player's camera 90 degrees and back.")
CFG_FLAG(pi2_selftest, "pi2_selftest", "dev", false, "VOTVCOOP_PI2_SELFTEST",
    "Autonomous selftest (BOTH peers): the locals a bytecode call writes, the puppet-count listener, the "
    "caller-locals boot check and the registry's puppet-body query, each one [PI-2] PASS or FAIL line, "
    "then [PI-2] DONE. Registers and clears a test puppet on a spare peer slot; reads the bytecode of "
    "three classes, loading any that is not yet loaded.")
CFG_ENUM(pi3_drill, "pi3_drill", "dev", "off", "off|wisp_w|wisp_o|wisp_p|kelura|orborb|orbbirth|grunt|control|client",
    "VOTVCOOP_PI3_DRILL",
    "Autonomous drill (BOTH peers): a creature or prop spawned at a still peer's puppet is not served as this "
    "machine's player, and the verbs it reaches are cancelled; [PI-3] PASS or FAIL then [PI-3] DONE, or "
    "[PI-3] ABANDONED. orbbirth: the host spawns a black orb and holds it; the client's mirror of it must not "
    "end that client's game at its birth ([PI-3] PASS or FAIL orbbirth, then DONE, printed by the client).")
