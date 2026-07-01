# Hands-on runbook — join-window MASS-MOVE pile dup fix (2026-07-01)

**Deployed DLL** `votv-coop.dll` sha256 `aa2249b163de` (all 4 folders hash-verified). Built Release.
Commit: the CONVERT-WINS fix (1 commit on top of the sound/deny commits).

## What changed (RE: research/findings/votv-joinwindow-massmove-dup-RE-2026-07-01.md)
Root of the 16:42 dup: when the host mass-grabs+throws piles DURING the join window, a save-load GC-churn
re-creates a fresh pile native `@save-pos` while the eid is already bound to the convert-landed **rooted
native `@new`**. `save_identity_bind` case(ii) only had a `PROXY-WINS` branch (fires when E's rendering is a
proxy); the **nativized** case fell through to a plain rebind that (a) reverted E to the stale `@old` and
(b) leaked the rooted landed native `@new` (no `RemoveFromRoot`) = the dup (old cluster + moved pile).

Fix: extend `PROXY-WINS` → `CONVERT-WINS` (also fires when E is bound to a convert-landed native), and harden
`ConsumeLocalActor` to un-root before destroy. The convert `@new` wins; the fresh save-native `@old` is retired.

## Test (2 peers; you run hands-on, Claude does NOT launch)
Reproduce the 16:42 scenario as closely as possible.

### T1 — host clears a cluster DURING the join window
1. Host: load a save that has a CLUSTER of chip piles in one spot. Start hosting.
2. Client: begin connecting. **While the client is still joining**, the host grabs + throws MANY of the
   cluster's piles to new spots (clear the original spot).
3. Client: finish joining and look at BOTH the original cluster spot AND the new spots.
4. **Expect (fix):** each moved pile appears ONCE, at its `@new` spot. The original cluster spot is EMPTY
   (no leftover dup piles `@old`). No piles hanging where they were cleared from.

### T2 — the EHHH is gone on the (now-recognized) host piles
1. Client: aim E at the moved host piles.
2. **Expect:** no `use_deny` "EHHH" — they're now bound/recognized mirrors, so the interceptor cancels the
   native use (the EHHH was a claim-state symptom; it disappears once identity is correct).

### T3 — clumps in air
1. After the mass move, look for any clump frozen in mid-air.
2. **Expect:** none (the ~2 seen at 16:42 were likely the same rooted-leak; report if any remain).

### T4 — no regression on a CLEAN join
1. Client joins while the host does NOT touch the cluster (clean join).
2. **Expect:** the cluster renders once, correctly (CtxForEid==0 → CONVERT-WINS does not fire → unchanged).

## Read in the client log
- `save_identity_bind: CONVERT-WINS ... landed native authoritative @new, redundant save-loaded native@old
  retired` — the NEW branch firing for a nativized in-window-moved pile (the fix working). Previously this
  eid took the plain `REBOUND mirror in place` path that leaked.
- Should NOT see a pile's eid land `@new` then immediately `REBOUND ... (morph re-skin)` to a fresh actor.
- `sweep-reconcile ABORTED ... >50%` should be rare/absent now (the dup source is killed at bind, so far
  fewer stale natives reach the sweep).

## Honest status
Built + deployed + hash-verified 4/4; NOT hands-on (needs the mass grab/throw join repro). The air-clump
resolution is a hypothesis (same rooted-leak) — confirm in T3. The >50% cap is retained as the fallback.
