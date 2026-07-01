# Hands-on runbook — join-window MASS-MOVE pile dup fix (2026-07-01, TAKE 2)

**Deployed DLL** `votv-coop.dll` sha256 `1C59BE738E04FDC9` (all 4 folders hash-verified). Built Release.
CLIENT-ONLY fix (the joiner runs the new logic; the host is unchanged). No protocol change.

## Why take 2 (17:50 re-derivation — 2 forensic agents on the full host+client logs)
The 17:50 test still showed the corner half-filled. Forensics (docs/piles/12) proved the prior two fixes
(`76257bb0` CONVERT-WINS, `46e35edd` twin-sweep) each fixed a survivor set that was NOT the corner. The corner
is the **completeness-FLOOR-kept** set: when the host mass-moves a cluster in the join window and the joiner's
snapshot bracket claims 0 of that class, the FLOOR (docs/piles/10) KEEPS the unclaimed local `@old` natives to
avoid a false >50% world-wipe — but registers them NOWHERE, so nothing ever retires them once the pile's `@new`
binds. They are `bound`-less save-loaded natives (GUI present, reconcile-on-interaction), so the unbound-only
twin sweep structurally never sees them.

## What changed (RE: research/findings/votv-joinwindow-massmove-dup-RE-2026-07-01.md, docs/piles/12)
`save_identity_bind::BindUnboundReCreates` — the client's quiescence re-bind loop already recovers a FLOOR
orphan's eid BY POSITION from the client's OWN save-time identity map (`g_chipEntries`, eid<->savePos; both
peers loaded the same save). Added the SYMMETRIC branch: for each map entry `{eid=E, savePos=@old}` whose E is
BOUND to a live native `>50cm` from `@old` (positive per-eid evidence E moved `@new`), if an UNBOUND native
still sits at `@old`, arm the save-time twin for E. The existing sweep then re-confirms the move and retires the
stale `@old` orphan PER-EID (confirmed -> NO cap). Runs in the post-purge drain window (a mass-move IS a purge
episode -> the drain is active). Verify-before-retire preserved: retire only on positive per-eid move evidence.

## Test (2 peers; you run hands-on, Claude does NOT launch)

### T1 — host clears a cluster DURING the join window (the repro)
1. Host: load a save with a CLUSTER of chip piles in one corner. Start hosting.
2. Client: begin connecting. **While the client is still joining**, the host grabs + throws MANY of the
   cluster's piles away (clear the original corner).
3. Client: finish joining, look at BOTH the original corner AND the new spots.
4. **Expect (fix):** the original corner is EMPTY (no leftover `@old` dups); each moved pile appears ONCE at
   its `@new` spot. Previously ~half the corner stayed filled with client-only dups.

### T2 — the misplaced/snap-on-pickup piles
1. If any pile is at a wrong spot, note whether it snaps to correct position on its own vs only when you grab it.
2. **Expect:** the DUP `@old` copies are gone (retired), not merely relocated. (A genuinely misplaced single
   pile whose convert was dropped — no dup, just wrong spot — is the SEPARATE B3 relocate axis, not this fix.)

### T3 — air-pickable pile with NO GUI prompt (SEPARATE track — just report)
1. Look for a pile floating in mid-air that has NO interact prompt but you can still pick up.
2. **Expect:** possibly still present (this is the materialize-FAILED -> NoCollision-proxy fallback track,
   Root-class 3 — NOT addressed by this fix). Report if seen; it's the next separate increment.

### T4 — no regression on a CLEAN join
1. Client joins while the host does NOT touch the cluster.
2. **Expect:** the cluster renders once, correctly. The DUP-RETIRE branch only fires when E's bound native is
   `>50cm` from its save-pos (i.e. the host actually moved it), so an untouched pile is never retired.

## Read in the client log (joiner)
- `save_identity_bind: DUP-RETIRE -- armed N save-time twin(s) from the identity map (eid bound @new but a
  stale UNBOUND native lingers @save-pos = FLOOR-kept mass-move dup, docs/piles/12)` — the NEW branch firing.
- Then `[PILE-1C] sweep-reconcile -- ... N confirmed-moved retired (per-eid, no cap)` on the next pass, with
  the count rising to cover the FLOOR-kept orphans (previously the FLOOR-6 never entered the sweep).
- `completeness FLOOR kept N unclaimed 'actorChipPile_C'` may still appear (the FLOOR still guards the wipe) —
  but those N should now get retired shortly after by the DUP-RETIRE + sweep, not linger.

## Honest status
Built + deployed + hash-verified 4/4; NOT hands-on (user present -> user runs the repro; Claude prepared
ground only). No autonomous smoke run (user on PC). If 1-2 dups still linger after T1 -> the GC pointer-reuse
residual (an `@old` re-bound to a WRONG eid escapes the unbound-only match) — a separate follow-up. T3
air-pickable is a known separate track.
