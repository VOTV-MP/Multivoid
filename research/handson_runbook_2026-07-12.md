# Hands-on runbook — 2026-07-12, TAKE 5 (placed-prop saga, roots 5+6)

Deployed DLL: **`0DA4FAE0`** (sha256 first 8), all 4 installs hash-verified. Supersedes take 4
(`3D250B83`, runbook 2026-07-11). Uncommitted at deploy: commit follows the audit verdict.

## What changed since take 4 (the "rock still gone" verdict, RCA §take-4)

1. **Wire-order queue netting (root 5).** The quiescence drain applied captured spawns then deferred
   destroys as PHASES, inverting the host's pickup->place (DESTROY->SPAWN) order for the same key and
   killing the placed rock at quiescence; it also resurrected rows a mid-episode wire destroy had
   legitimately killed. Now the queues are pre-netted per identity at capture: a wire DESTROY cancels
   the captured spawn of its incarnation; a later wire SPAWN supersedes a pending deferred destroy.
2. **setKey SuperStruct climb (root 6).** Take-3's duplicate-Key re-key authority was INERT: all 162
   re-keys failed ("setKey not found on trashBitsPile_C") because setKey is declared on the
   actor_save_C ancestor and FindFunction is exact-owner. The resolver now climbs the chain.

## Test steps (same repro as takes 2-4)

1. Host: load the usual save ("Host game"). Client: connect; WHILE the client sits in the connect /
   world-load window, host hold-R a world rock into the hotbar and place it back somewhere visible.
2. After the client is in: check the placed rock EXISTS on the client, at the host's placement spot.
3. Take-2 regression: a rock from pre-connect hotbar stock placed in-window should also appear.
4. Same-spot steal gate + join regression (mushrooms/garbage not doubled) — quick glance.
5. fps sanity post-join (take-3's 2.5fps class).

## What to read in the logs

HOST (first load of the poisoned save):
- `KEY-UNIQUENESS ... re-keyed -> 'rk_...'` burst (WARN, ~85-162 lines) — root-6 fix WORKING.
  `re-key FAILED` lines = still broken, stop and report.
- Next boot after the game re-saves: the burst drops to ~0.

CLIENT (join with the in-window pickup/place):
- `[SPAWN-DEFER] CLIENT CANCELLED captured in-episode spawn ... a later wire DESTROY` — destroy-side
  netting fired (the old incarnation will not resurrect).
- `[DESTROY-DEFER] CLIENT SUPERSEDED deferred destroy ... by a later same-identity wire spawn` — the
  stale destroy will NOT kill the placed rock. This is the load-bearing line for the repro.
- NO `dead keyed mirror row SURVIVED` tripwire for the placed prop's key.

## Honest status

Take-4 build (`3D250B83`) FAILED the user repro (rock gone); this build fixes the two roots that log
exposed. Compiles + deployed + hash-verified; NOT smoked (user at PC — user tests per the standing
rule). Audit pass running at handoff; verdict lands before/with the commit.
