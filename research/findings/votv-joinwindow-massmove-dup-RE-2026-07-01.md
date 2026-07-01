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

## Status
Built Release, deployed `aa2249b163de` (4/4 hash-verified). NOT hands-on yet — needs the mass grab/throw
join-window repro. Expect: no dup cluster (E stays @new), the host's piles recognized → no EHHH, and (likely)
the air-clumps gone (same rooted-leak). Runbook: `research/handson_runbook_2026-07-01_massmove_dup.md`.
