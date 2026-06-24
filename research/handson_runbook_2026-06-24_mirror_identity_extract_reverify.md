# Hands-on runbook -- mirror-identity extract (b6fb2638) NO-REGRESS re-verify (3 tests)

**Purpose:** the shared-kernel extraction (`coop/save_time_retire_util.h` -- `FindExactMatch` +
`UnmarkAndDestroy` + `kExactMatchR2Cm`) refactored TWO verified reconcile instances
(pile_reconcile + kerfur_reconcile). This run confirms it did NOT regress any of the THREE verified
mirror-identity instances. It is a behavior-preservation check, not a new feature.

**Deployed:** MD5 `510fbd28` (short) on HOST (`Game_0.9.0n`) + CLIENT (`Game_0.9.0n_copy`) + CLIENT2
(`Game_0.9.0n_copy2`) + DEV (`Game_0.9.0n_dev`). **Proto v88** (UNCHANGED -- the extract touched no
protocol; both peers run the same DLL). Build clean Release. HEAD `b6fb2638` (1 ahead of origin/main
`24ee5220`, push HELD until all 3 PASS).

**What changed (and why no regress is expected):** the 1cm exact-match + claim-track + ambiguous-skip +
UnmarkKnownKeyedProp+DestroyActor are now ONE shared kernel both sweeps call. The per-class seams stay
in each .cpp: the pending map, the class predicate, the kerfur mirror-exclusion, and the >50% ratio
valve (pile keeps it; kerfur has none -- the 17:06 lesson, the valve is NOT in the shared header). One
transparent delta: the pile SWEEP loop now does explicit ambiguous(>1)->skip (was break-on-first) --
identical on the real position-unique path, strictly safer in the unreachable two-piles-in-1cm case.

---

## TEST 1 -- L1 pile dup (pile_reconcile sweep)
Setup + steps: `research/handson_runbook_2026-06-23_pile_1c_dup.md` (the STRICT gate). Host moves a
save-loaded pile in the client's join-load window.
**Acceptance (must match the pre-extract PASS byte-for-byte):**
- visual: 4 piles, NO dup.
- host log: `P>0` (save-time xforms captured).
- client log: `[PILE-1C] sweep-reconcile -- N of M pending save-time twin(s) removed at post-quiescence`
  (the format string is UNCHANGED -- if it reads differently the extract regressed the log).
- NOT the loc-fallback false-pass: require the `[PILE-1C] sweep-reconcile` line, not a census-only pass.

## TEST 2 -- kerfur fuzzy-gate collision (kerfur_prop_adoption -- instance 2, NOT migrated)
Setup + steps: `research/handson_runbook_2026-06-24_kerfur_fuzzygate_collision.md`. Host turns a kerfur
OFF in the window (active-at-blob -> off cluster).
**Acceptance:** 5 DISTINCT kerfur-prop actors (no 2-eid-on-1-actor collision); both peers 5-off/1-active.
**Note:** instance 2's code was NOT touched by the extract -- this is a pure non-regression smoke (the
shared header only changed the two SWEEP instances). A PASS here just confirms nothing adjacent broke.

## TEST 3 -- kerfur forward off->active (kerfur_reconcile sweep = scope A)
Setup + steps: `research/handson_runbook_2026-06-24_kerfur_scopeA_retire.md`. Host turns an OFF-in-save
kerfur ON in the window.
**Acceptance (must match the 17:23 PASS byte-for-byte):**
- client log: `kerfur_reconcile: ARMED off->active retire eid=... (from npc EntitySpawn ...)` then
  `kerfur_reconcile: sweep-retire -- 1 of 1 pending save-time kerfur retire(s) at post-quiescence`
  (format UNCHANGED).
- the off-from-save kerfur DISAPPEARS; client total == host (6, not 7); the active NPC is the sole form.

---

## After the run
On ALL 3 PASS (log lines byte-identical + no dup + no collision): the extract is no-regress -> push
`b6fb2638` (with scope A already in origin). On ANY fail: paste the failing test's log lines + which
format string differs / what duplicated; do NOT push. The extract is a refactor, so a regress means a
seam was mis-drawn -- diagnose against the pre-extract behavior (HEAD 24ee5220), no blind fix.

**Note:** the held L5 instrumentation (interactable_channel.h + subsystems.cpp + device_screen.cpp) is
compiled into `510fbd28` exactly as it was in the 17:23 scope-A-PASS build -- it is orthogonal to
pile/kerfur reconcile and is the same baseline, so it does not confound this A/B.
