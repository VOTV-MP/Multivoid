# VOTV join-window MASS-MOVE pile dup — RE + root fix — 2026-07-01

Hands-on 16:42 (client log `Game_0.9.0n_copy`): the host, DURING the client's join window, mass-grabbed
and threw a whole cluster of piles (clearing the spot). The client finished joining and saw **both the
old cluster (@old positions) AND the moved piles (@new)** = a dup cluster, plus ~2 clumps stuck in air.
Secondary: EHHH (native `use_deny`) on the host's real piles but not on the dups — a pure claim-state
symptom (dups are bound→recognized→interceptor cancels→no deny; the host's real moved piles came in
unrecognized→native denies). The deny fix (dc8bd6af) is orthogonal; the EHHH resolves when identity is fixed.

Probe-first (grep before hypotheses). The diagnosis evolved through the log and corrected two wrong guesses.

## What the logs proved

- `CLAIMED bound save-loaded` = **0** — the create-edge LAND claim (fa8bc344) never engaged this run, so the
  dup is NOT a LAND-claim failure.
- `[16:41:18] sweep-reconcile ABORTED -- 5 save-time twin removals of 5 live native(s) (>50%) -- keeping all
  natives` — the >50% anti-world-wipe valve ([[feedback-join-reconcile-sweep-safety]]) tripped: in a mass
  clear ~100% of the region's live natives are legitimately stale, so removing them reads as a wipe → abort →
  stale natives kept. This is a **secondary** symptom (a fallback that can't mop up), not the engine.
- Air-clumps: NOT leaked proxies — every ToClump got a matching ToPile (`stuck-as-clump eids = []`), and the
  clump proxies traced were all torn (`RETIRE-ACTOR-ONLY`). Source unpinned at diagnosis time; the root fix
  below (rooted-leak) is the likely cause — verify in hands-on.

## Root (fully pinned, eid 4958)

```
16:41:11  ToPile LAND eid=4958 ctx=2 -> NATIVIZED native=8C72C680 @new (3073,-1066)   [convert placed E @new]
16:41:12  CreateOrAdoptPropMirror: eid=4958 REBOUND mirror in place -> actor=49111500 (morph re-skin) [reverts @old]
```
A save-load GC-churn re-created a fresh actorChipPile native `@save-pos` (`49111500`) while eid 4958 was
already bound to the convert-landed **rooted materialized native `8C72C680 @new`**. In
`save_identity_bind::OnSaveLoadSpawn` case (ii):
- The **`PROXY-WINS`** branch (converted → convert rendering authoritative → retire the redundant fresh
  save-native) fires **only when `IsProxy(E)`**. Since nativization (2026-06-30) a ToPile LAND materializes a
  rooted NATIVE, so E is bound to a native, not a proxy → `IsProxy(E)` is false → **fell through** to the plain
  rebind, which did BOTH wrong things (one hole, two ends):
  1. **re-pointed E to the stale-save-pos native `@old`** → the old cluster became the visible mirror; and
  2. `ConsumeLocalActor(oldActor)` on the rooted landed native → **no `RemoveFromRoot`** (unlike `OnDestroy`,
     which un-roots) → `K2_DestroyActor` only sets PendingKill while `AddToRoot` keeps it **alive `@new`** = the
     second representation.

`ConsumeLocalActor` (remote_prop_destroy.cpp) confirmed: `MarkIncomingDestroy` + `K2_DestroyActor`, no un-root.

## Fix (per rule 1 — convert-authority at the real seam)

1. **`PROXY-WINS` → `CONVERT-WINS`**: extend the converted-authoritative branch to fire when E's current
   rendering is a proxy **OR** a bound convert-landed native — `... && CtxForEid(E) > 0 && (IsProxy(E) ||
   PT::IsBoundMirrorNative(oldActor))`. Body unchanged: retire the fresh stale-save-pos native, keep E on its
   authoritative `@new` rendering. Kills the dup at the root (no revert to @old, no orphaned native).
2. **Harden `ConsumeLocalActor`**: `R::RemoveFromRoot(actor)` before `K2_DestroyActor` (defense-in-depth, the
   same un-root `OnDestroy` already does; no-op on an unrooted game-native). Any rooted native reaching a
   consume no longer leaks.

The `>50%` cap STAYS as the fallback for true no-convert-evidence stale natives (verify-before-retire: the cap
was born from a real world-wipe; it's relieved of load, not removed).

## Status (CONVERT-WINS, aa2249b163de) -- INSUFFICIENT
Hands-on 17:10: did NOT fix the corner (still filled). CONVERT-WINS fired only 4x (all viaProxy); it
addressed a real-but-minor sub-case, not the dominant one.

## CORRECTED ROOT (2 read-only forensic agents + census converged, 17:10 log)
The dup is exactly the ~5 piles the host moved in-window: census CLIENT 874 vs HOST 869 = 5 extras. At
17:09:53 THREE guards each named "5" and KEPT the stale native@old:
`join_membership_sweep: completeness FLOOR kept 5 unclaimed 'actorChipPile_C'`,
`[PILE-1C] sweep-reconcile ABORTED -- 4 twin removals of 5 live native(s) (>50%) -- keeping all natives`,
`save_identity_bind: OVERFLOW -- 5 chipPile spawn(s) exceeded the mapped count`.
**Primary = the aggregate >50% cap on SweepReconcileSaveTimeTwins.** A legit mass-move (host clears a cluster)
makes the moved piles >50% of live natives -> the cap reads it as a racing/incomplete-bracket world-wipe ->
aborts -> the 5 stale @old twins survive. **Worse: the sweep then g_pendingSaveTimeTwin.clear()'d the twin map
-- and it ran ~4s BEFORE the moved piles' @new PropSpawn+materialize arrived (17:09:53 sweep vs 17:09:57 @new),
so at sweep time NONE were confirmable, and clearing them meant no later pass could ever retire them.**
(Compounding: the twin-sweep only retires UNBOUND natives, but GC pointer-reuse RE-BIND re-bound some @old
natives to wrong eids, e.g. 4965<->4967, so they were never in the doom set.) CONVERT-WINS missed this because
it only touched the save_identity_bind bind-seam, not the twin sweep.

## FIX (per-eid confirmed-move retire; cca97fa3c93f)
Rewrote SweepReconcileSaveTimeTwins (quiescence_drain.cpp): split each pending twin into CONFIRMED-moved
(E's currently-bound native lives >50cm from the twin's save-pos = the host moved E @new, positive per-eid
evidence -> retire the stale @old twin with NO aggregate cap) vs UNCONFIRMED (E not yet bound @new). The >50%
cap now applies ONLY to the unconfirmed remainder (the racing-bracket case it was born for), and
unconfirmed/unmatched twins are KEPT pending (bounded by kMaxTwinPasses=40 ~10s) instead of cleared -- so the
NEXT drain pass, once @new binds E, confirms the move and retires @old per-eid. This is "per-eid convert IS the
proof; the cap becomes the fallback." Deployed cca97fa3c93f (4/4). RESIDUAL (honest): the GC pointer-reuse
class (1-2 of 5, @old re-bound to a wrong eid -> excluded by the UNBOUND-only walk) may persist -- separate
follow-up if hands-on still shows 1-2. Runbook: research/handson_runbook_2026-07-01_massmove_dup.md.
