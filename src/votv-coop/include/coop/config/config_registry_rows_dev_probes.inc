// coop/config/config_registry_rows_dev_probes.inc -- the [dev] diagnostic probes and censuses that sat
// together in the dev row list, moved in their order: the place/birth key probe, the spawn match probe, the lifetime trace, the pinecone and
// store readouts, the hotbar icon readout, the heap, drone and pile probes, the performance probe and its
// variants, the light probes and census, the script gate log, the RNG roll census and the leak probe. Included by
// config_registry_rows_dev.inc where they stood, so each row keeps its place in every expansion of the list. The
// entry forms are config_registry_rows.inc's.

CFG_FLAG(prop_birth_key_probe, "prop_birth_key_probe", "dev", false, "VOTVCOOP_PROP_BIRTH_KEY_PROBE",
    "Diagnostic probe (both roles): the place/birth seam -- how many drain ticks the save Key takes "
    "to restore after a spawn, and which of the seven drain exits each pending spawn leaves by; and "
    "every keyed prop it saw born that a script then destroys, with the session's state at the "
    "destroy, even with no session. The drain exits are the client's; the host places through a "
    "different path.")
CFG_FLAG(spawn_match_probe, "spawn_match_probe", "dev", false, nullptr,
    "Diagnostic probe: the OnSpawn fuzzy-match candidate set -- every same-class prop inside the "
    "30 cm radius with its slot, distance, own key and mirror binding, plus a watch that reports a "
    "destroy landing on an adopted actor. Walks the whole object array per fuzzy spawn while on.")
CFG_FLAG(eid_lifetime_trace, "eid_lifetime_trace", "dev", false, nullptr,
    "Diagnostic trace: entity-id lifetimes (mint/adopt/destroy).")
CFG_FLAG(pinecone_probe, "pinecone_probe", "dev", false, nullptr,
    "Diagnostic probe: pinecone prop behavior.")
CFG_FLAG(live_store_readout, "live_store_readout", "dev", false, nullptr,
    "Diagnostic readout: the live GObjStack container store.")
CFG_FLAG(hotbar_icon_probe, "hotbar_icon_probe", "dev", false, "VOTVCOOP_HOTBAR_ICON_PROBE",
    "Diagnostic readout: the quick-slot bar at world load -- the slot widget arrays, the "
    "prop-icon name table and texture cache, the carried count, and per slot which texture is bound "
    "to its `tex` parameter. Logs on change, so a settled bar is silent. The env twin exists because "
    "the question is whether the bar loses the race on ONE peer or both, which an ini-only row "
    "cannot arm in a single run.")
CFG_FLAG(heap_probe, "heap_probe", "dev", false, nullptr,
    "Diagnostic probe: process heap usage.")
CFG_FLAG(drone_probe, "drone_probe", "dev", false, nullptr,
    "Diagnostic probe: delivery drone state.")
CFG_FLAG(drone_probe_drive, "drone_probe_drive", "dev", false, nullptr,
    "Diagnostic probe: drive the delivery drone (test driver).")
CFG_FLAG(pile_dup_probe, "pile_dup_probe", "dev", false, nullptr,
    "Diagnostic probe: pile duplicate/identity mismatches.")
CFG_FLAG(perf_probe, "perf_probe", "dev", false, "VOTVCOOP_PERF_PROBE",
    "Performance probe: per-frame subsystem timing lines, the reflected-call rate and the ParamFrame allocation rate. The env twin exists so a launcher can arm BOTH peers for one run: an ini-only dev row has to be edited per install, which is how a perf number ends up measured on one peer and assumed on the other.")
CFG_FLAG(perf_probe_selftime, "perf_probe_selftime", "dev", false, nullptr,
    "Performance probe: self-time breakdown variant.")
CFG_FLAG(perf_probe_dispatch, "perf_probe_dispatch", "dev", false, "VOTVCOOP_PERF_PROBE_DISPATCH",
    "Performance probe: count every ProcessEvent dispatch (PE/s, game-thread share, our share). OFF by default -- gameplay dispatches ~170k/s across several threads, so this is a shared-atomic RMW on a contended cache line at that rate. Leave it off whenever the number being measured is what the mod itself costs; the 1 Hz fps line does not need it.")
CFG_INT(perf_probe_resdrop, "perf_probe_resdrop", "dev", 0, 0, 3600, nullptr,
    "Performance probe: after N 1 Hz samples, collapse render resolution (r.ScreenPercentage 25) to separate a GPU-bound frame from a CPU-bound one.")
CFG_INT(perf_probe_bypass, "perf_probe_bypass", "dev", 0, 0, 3600, nullptr,
    "Performance probe: after N samples, arm the transparent ProcessEvent bypass for 5 s (separates our dispatch-path cost from everything else about our presence).")
CFG_INT(perf_probe_statunit, "perf_probe_statunit", "dev", 0, 0, 3600, nullptr,
    "Performance probe: after N samples, issue `stat unit` + r.VSync 0 + t.MaxFPS 0. OFF by default because enabling any UE4 stat turns on FThreadStats collection engine-wide -- an instrument that changes the frame it measures. The delay exists so the samples before it are the uninstrumented baseline and the step across this line IS the instrument's own cost.")
CFG_FLAG(lightswitch_probe, "lightswitch_probe", "dev", false, "VOTVCOOP_LIGHTSWITCH_PROBE",
    "Diagnostic probe: light switch interactions + the light-group apply selftest. MUTATES the world (one-shot synthetic use()) -- never enable it on two peers at once. The env var exists so `mp.py lightgroup` can arm it: an ini-only dev row is unreachable to every launcher, which is how this selftest went unrun after a refactor.")
CFG_FLAG(lightgroup_census, "lightgroup_census", "dev", false, "VOTVCOOP_LIGHTGROUP_CENSUS",
    "Diagnostic census: per-key light GROUP state (isActive / active gate / buffIsActive) + switch A. Read-only; enable on BOTH peers and diff by key. It has an environment twin so a run can turn it OFF as its named variable: it dumps a line per group on a period, and any measurement of a per-frame or per-second effect has to be able to rule that out.")
CFG_FLAG(script_gate_log, "script_gate_log", "dev", false, nullptr,
    "Diagnostic logging for the script-body gate's consumers (the kerfur form assembler's per-verb lines).")
CFG_FLAG(rng_roll_census, "rng_roll_census", "dev", false, nullptr,
    "Diagnostic census: RNG roll sites.")
CFG_FLAG(server_breaker_census, "server_breaker_census", "dev", false, "VOTVCOOP_SERVER_BREAKER_CENSUS",
    "Diagnostic census (BOTH peers): every 30 s the server breaker's inputs in one line (its ticker, the game mode byte, the difficulty, the day, the server list against the live boxes, the broken count against the boxes actually broken), and a line per breakServer, fix and break_type. Read-only.")
CFG_FLAG(leak_probe, "leak_probe", "dev", false, nullptr,
    "Diagnostic probe: object/memory leak counters.")
