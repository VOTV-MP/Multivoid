# 12 — Join-window MASS-MOVE pile dup (2026-07-01)

**Status: AS-BUILT, UNDER HANDS-ON.** The primary fix (per-eid confirmed-move twin retire, `46e35edd`) is
built + deployed `cca97fa3c93f` (4/4 hash-verified) but NOT yet hands-on-verified. A prior attempt
(CONVERT-WINS, `76257bb0`) proved INSUFFICIENT. Full RE + timeline:
`research/findings/votv-joinwindow-massmove-dup-RE-2026-07-01.md`. This is a SEPARATE class from `docs/piles/09`
(single held-clump-at-join) and `docs/piles/11` (nativization) — it is the MASS version. [[project-pile-nativization-2026-06-30]]

## Symptom (hands-on 16:42 + 17:10)
The host, DURING the client's join window, mass-grabs+throws a CLUSTER of chipPiles (clears a building
corner). The client finishes joining and sees the **corner still FILLED** = stale piles `@old`. Census:
CLIENT 874 vs HOST 869 keyless chipPiles = **~5 client-only dups**. (A secondary EHHH `use_deny` on those
piles is a claim-state symptom — unrecognized/unbound → the grab interceptor returns false → native denies —
NOT a sound bug; it clears when identity is fixed.)

## Root (2 read-only forensic agents + census converged) [RD + log-V]
At 17:09:53 THREE guards each named "5" and KEPT the stale `native@old`:
```
join_membership_sweep: completeness FLOOR kept 5 unclaimed 'actorChipPile_C'   (docs/piles/10 guard)
[PILE-1C] sweep-reconcile ABORTED -- 4 twin removals of 5 live native(s) (>50%) -- keeping all natives
save_identity_bind: OVERFLOW -- 5 chipPile spawn(s) exceeded the mapped count
```
**Primary = the aggregate `>50%` cap on `SweepReconcileSaveTimeTwins`.** A legit cluster-clear makes the moved
piles >50% of the region's live natives → the cap reads it as a racing/incomplete-bracket world-wipe → aborts
→ the stale `@old` twins survive. **Worse: the sweep then `g_pendingSaveTimeTwin.clear()`'d the twin map, and
it ran ~4 s BEFORE the moved piles' `@new` PropSpawn+materialize arrived (sweep 17:09:53 vs `@new` 17:09:57)** —
so at sweep time none were confirmable, and clearing them meant no later pass could retire them. Compounding:
the sweep only retires UNBOUND natives, but a GC pointer-reuse `RE-BIND-by-position` re-bound some `@old`
natives to WRONG eids (e.g. `4965↔4967`), so they were never in the doom set.

Recurred after 2 targeted fixes (fa8bc344 create-edge claim, 76257bb0 CONVERT-WINS) → the patch LEVEL was
wrong; re-derived from logs per [[feedback-recurring-bug-is-architectural]].

## Fix — per-eid CONFIRMED-move twin retire (`46e35edd`) [AS-BUILT]
Rewrote `SweepReconcileSaveTimeTwins` (`quiescence_drain.cpp`):
- Split each pending twin into **CONFIRMED-moved** — E's currently-bound native lives `>50cm` from the twin's
  save-pos = the host moved E `@new`, positive per-eid evidence → retire the stale `@old` with **NO aggregate
  cap** — vs **UNCONFIRMED** (E not yet bound `@new`).
- The `>50%` cap now applies **ONLY to the unconfirmed remainder** (the racing-bracket case it was born for).
- **Unconfirmed/unmatched twins are KEPT pending** (bounded, `kMaxTwinPasses=40 ≈ 10 s`) instead of cleared —
  so the NEXT drain pass, once `@new` binds E, confirms + retires `@old` per-eid.

"Per-eid convert IS the proof; the `>50%` cap becomes the fallback" — the design the user proposed. Preserves
the world-wipe protection (verify-before-retire, [[feedback-join-reconcile-sweep-safety]]).

## RESIDUAL / open
- **GC pointer-reuse class (1–2 of 5):** an `@old` native re-bound to a WRONG eid is excluded by the
  UNBOUND-only walk → may persist. Separate follow-up if hands-on still shows 1–2 after the primary fix.
- **Air-clumps (≤2, mid-air):** a SEPARATE track (mid-air Z, not the ground `@old` dup). NOT a leaked proxy
  (the traced clump proxies were all `RETIRE-ACTOR-ONLY`'d). Leading candidate: the `known=0 carry pose holds
  forever` pattern (a clump proxy driven by a stale/ahead carry-pose stream whose convert-gen was never
  adopted). Diagnose separately.
- **Superseded here:** the CONVERT-WINS extension (`76257bb0`, `save_identity_bind` `PROXY-WINS`→`CONVERT-WINS`
  + `ConsumeLocalActor` un-root) is CORRECT but was insufficient alone (fired only on the proxy sub-case). It
  stays (defense-in-depth); the twin-sweep rewrite is the actual mass-move fix.

## Verify (hands-on)
Runbook `research/handson_runbook_2026-07-01_massmove_dup.md`. Expect: the corner CLEARS (each moved pile once,
`@new`; original spot empty), EHHH gone on the moved piles, clean-join no regression. Log:
`[PILE-1C] sweep-reconcile -- N confirmed-moved retired (per-eid, no cap) ...`. If 1–2 dups linger → the
pointer-reuse residual.
