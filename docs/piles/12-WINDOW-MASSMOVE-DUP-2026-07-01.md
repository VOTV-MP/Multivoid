# 12 — Join-window MASS-MOVE pile dup (2026-07-01)

**Status: AS-BUILT (take 2), UNDER HANDS-ON.** Deployed `1C59BE738E04FDC9` (4/4 hash-verified). This is a
CLIENT-ONLY fix. Prior attempts `76257bb0` (CONVERT-WINS) and `46e35edd` (twin-sweep per-eid confirmed-move)
each targeted a survivor set that was NOT the corner (17:50 hands-on re-derivation below). Full RE + timeline:
`research/findings/votv-joinwindow-massmove-dup-RE-2026-07-01.md`. This is a SEPARATE class from `docs/piles/09`
(single held-clump-at-join) and `docs/piles/11` (nativization) — it is the MASS version. [[project-pile-nativization-2026-06-30]]

## 17:50 hands-on re-derivation (2 forensic agents on the full host+client logs) [log-V]
The 46e35edd twin-sweep DID run (hash-verified) and behaved correctly (`27 pending -> 5 confirmed-moved retired
-> settles to 2 stuck`, both stuck = already-converged eids mid-countdown at shutdown, NOT dups). But the corner
was a DIFFERENT survivor set: `join_membership_sweep: completeness FLOOR kept 6 unclaimed 'actorChipPile_C'
(claimed 0 this bracket, INCOMPLETE snapshot)`. Those 6 are BOUND-less save-loaded natives at @old that the
FLOOR (docs/piles/10) KEEPS to avoid a false >50% world-wipe — but `join_membership_sweep.cpp:276` does
`++floorKeptByClass; continue;` and registers them NOWHERE. `ArmPendingSaveTimeTwin` has two callers
(`pile_spawn_bind.cpp`, `remote_prop.cpp` convert LAND) — neither is the FLOOR. So a FLOOR-kept orphan is
**terminal**: it only dies if an independent event happens to key onto its exact position (luck of overlap). The
three prior fixes all patched the event-keyed convert lane, which the joiner's pre-world gate can DROP. Root
(patch level): **the FLOOR holds the orphan but never registers it for deferred retire** — the symmetric hole to
"a convert arrives but nobody applies it". The other two 17:50 symptoms are SEPARATE and NOT this dup: B3
misplaced-snap (applied clean, drift 0 this run) and the air-pickable no-GUI pile (materialize-FAILED ->
NoCollision proxy fallback; own track, below).

## Fix take 2 — CLIENT-ONLY DUP-RETIRE from the save-identity map (`save_identity_bind.cpp`) [AS-BUILT]
The FLOOR orphan is UNBOUND, so it has no eid to arm a confirmable twin with — BUT the client holds its OWN
save-time identity map (`g_chipEntries`, eid<->savePos, sidecar v2; both peers loaded the identical save). The
re-bind loop at `BindUnboundReCreates` already recovers a FLOOR orphan's eid BY POSITION from that map, but only
when the eid is FREE. Added the SYMMETRIC branch: for each map entry `{eid=E, savePos=@old}` whose E is BOUND to
a live native `>50cm` from @old (positive per-eid evidence E moved @new), if an UNBOUND native still sits at @old
-> that native is E's stale @old twin -> `ArmPendingSaveTimeTwin(E, @old, chipType)`. The existing sweep then
re-confirms E moved and retires the orphan PER-EID (confirmed -> NO cap). Runs in the post-purge drain window (a
mass-move IS a purge episode -> `InPurgeEpisode` -> 6s window -> the drain runs). No host change, no protocol
change, no FLOOR change (the FLOOR still guards the wipe; we resolve what it kept). Verify-before-retire preserved
([[feedback-join-reconcile-sweep-safety]]): retire only on positive per-eid move evidence.

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
