# autotest.cpp catch-all dissolve -- the island's one-feature-per-file convention applied (2026-07-19, AS-BUILT)

The s25-queue "go next" item: `src/harness/autotest/autotest.cpp` measured **1002 LOC**
(soft cap 800; audit-flagged "split per-feature test routines"). Dev-only test island
(RULE 3: never ships). 4-round /qf converged ("that holds" round 4).

## Axis (settled by measurement)

The island's OWN convention: 17 sibling files, one feature per `autotest_*.cpp` (56..1013
LOC each), all declared in the flat umbrella `include/harness/autotest.h`, all dispatched
from `autotest_dispatch.cpp`'s `SpawnIf(env, label, thread)` table. autotest.cpp was the
residual catch-all holding 10 routines. Measured facts the cut rests on:
- ZERO shared mutable state between routines (every routine's state = local
  `shared_ptr<atomic>`; only shared symbols were the anon-namespace ReadEnv + aliases).
- Sole external consumer = the dispatch table via `&XxxThread` fn ptrs (caller census: no
  other references anywhere in src/).
- The local ReadEnv copy was BYTE-IDENTICAL to `coop::config::ReadEnv` (config.cpp:38);
  newer siblings (dispatch/alarmforce/cueforce/eventfire) already use config's.

## The three commits (extract-then-rename, /qf R2-R3 finds)

- **`89ce6602` commit 0** -- ReadEnv retirement ALONE, in-place (8 call sites -> 
  `cfg::ReadEnv`, local helper deleted). Isolated so the extraction diff stays pure
  verbatim motion (the swap would otherwise blind the body-diff instrument).
- **`f299107c` commit 1** -- the 9-routine extraction, verbatim spans:
  `autotest_clump.cpp` (200: held-clump ATTACH e2e + clump visibility probe),
  `autotest_weather.cpp` (198: forced-rain cycles + red-sky variant),
  `autotest_flashlight.cpp` (154), `autotest_worldrules.cpp` (69),
  `autotest_worldctx.cpp` (49), `autotest_tracker_selftest.cpp` (77: prop-reap
  self-test + re-seed probe -- ONE subsystem, prop_element_tracker; the /qf R1
  cross-subsystem "selftests" bundle was rejected, worldctx got its own file).
  Residual = grab-only 393. autotest.h doc comments re-anchored; 8 now-unused residual
  includes pruned (enumerated); CMake rows added. DROP_OK: the 3-line saveui
  location-pointer comment (role owned by autotest.h).
- **`cc4c93c3` commit 2** -- PURE `git mv autotest.cpp -> autotest_grab.cpp` (99%
  similarity, rename detected, `--follow` history survives). A fused mv+strip would have
  landed at ~41% similarity -- under git's 50% rename threshold (the /qf R1 find).
  autotest.h top comment now states its real role: the island's umbrella interface.

## Equivalence evidence

- **Literal body-diff** (`scratchpad/autotest_cut/body_diff.py`): 6 spans (556 non-blank
  lines) verbatim-contiguous in their destinations; destination extras = enumerated
  scaffold only; residual == baseline-minus-spans-minus-enumerated-drops EXACTLY
  (sequence equality -- subsumes the moved-line-leak check). **Mutate controls: 7**
  (one per destination + one in the residual grab span), each independently FAIL;
  clean run PASS.
- **Runtime differential, ALL TEN routines exercised** (the s25 smoke-env-passthrough
  lesson, extended): two scenario sets chosen by the ONE-WRITER-PER-AXIS invariant
  (grab+clump contend on the host held-item axis -- measured overlapping windows
  10-70s vs 35-60s; clump+clumpvis contend on the garbage-clump wire lane -- both spawn
  `prop_garbageClump_C`, the clump-test client literal would be ambiguous; redsky
  measured NOT to touch the rain flags -- it writes only its own actor at gm+0x0888):
  - PAIR A (8 scenarios: WEATHER REDSKY FLASHLIGHT WORLDRULES WORLDCTX PROPREAP RESEED
    CLUMP): 27 verdict keys -- baseline vs post IDENTICAL, including the
    timing-sensitive flash.applied counts and the full clump chain
    (host spawn -> client mirror OnSpawn -> kinematic).
  - PAIR B (GRAB CLUMPVIS): 9 verdict keys IDENTICAL (grab full chain:
    resolve OK -> PHC.Grab hook -> PHC.Release hook; clumpvis READY+DONE both peers).
  - Baselines ran on POST-commit-0 bytes (DLL `dc4e100f3c0ebfcc` x4) so the runtime
    differential isolates the pure move; post runs on final bytes. Identical restored
    `s_1234.sav` + identical shell env per pair; same gate.sh extractor both sides.
  - Only informational drift: non-perf warn counts (118->116 / 57->59 / 121->122),
    same warn CLASSES both runs (fingerprint compare); one known join-window timing
    class (`join_membership_sweep: claim sweep ABORTED`, 0->1 on the pair-A client) --
    a protective self-healing guard on a path the diff does not touch.
- Deploy: final DLL **`b62c64263f8075f0`** x4 hash-verified, proto 121 unchanged
  (no wire change; dev-only code).

## Landings (live wc -l)

autotest_grab 393 (was autotest.cpp 1002) · clump 200 · weather 198 · flashlight 154 ·
tracker_selftest 77 · worldrules 69 · worldctx 49. Island: 17 -> 23 files, every one
under the soft cap EXCEPT the pre-existing autotest_vitals.cpp 1013 + autotest_chippile.cpp
877 (untouched; flagged -- vitals proposal: the PuppetFrame nameplate capture rig is not a
vitals test, split it out + the damage-family trio can follow the same one-feature shape;
chippile is a single-feature family file, borderline-exempt, watch it).

## Audit (code-reviewer agent, post-commit)

7/7 PASS: moves faithful (33 Run*/Thread pairs, each defined exactly once; zero
duplicates/orphans across src/), include hygiene clean (one ~50-confidence nit:
autotest_clump.cpp gets <cstdint> transitively via call.h), ReadEnv swap exact
(8 sites re-counted independently 1+1+1+2+3; semantics corroborated against the
still-local copies in saveui/vitals), CMake 23 files exactly once, dispatch
references all 33 Thread fns 1:1, all touched files far under the soft cap,
nothing on a hot path. ONE finding >=80: three stale "used by" comments still
pointing at the retired harness/autotest.cpp path (harness.cpp:140,
engine_mainplayer.cpp:19, item_activate.h:115) -- FIXED in `1a9a4258`
(comment-only; negative grep = zero retired-path references remain outside the
new TUs' lineage notes; DLL bytes unaffected).

## Honest status

Behavior preservation is literal-diff + differential-smoke proven; this is
dev-only harness code (never ships), so there is nothing hands-on to verify beyond the
smokes -- the runtime differential exercised all ten routines end-to-end cross-peer.
