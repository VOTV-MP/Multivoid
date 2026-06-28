# Hands-on runbook — D1 + D2 verify (one run, 2026-06-28)

> **SUPERSEDED / DEFERRED 2026-06-28 (same day).** This hands-on was CANCELLED as PREMATURE: the user
> decided the sync module is mid-refactor (half moved to `coop/sync/`, half not), so testing the
> half-assembled state catches transitional noise, not real bugs. A first run (16:24) showed exactly that:
> the adopt-by-eid WORKED (every kerfur toggle `adopted ... by eid`, zero fuzzy-miss/timeout) but downstream
> symptoms appeared (kerfur twitch + hang-in-air on turn-off; pile host-only). Those 3 symptoms are PARKED in
> the PLAN-doc "OPEN SYMPTOMS" ledger to RE-CHECK after the module is whole. The D1/D2 + 3-symptom hands-on
> happens on the ASSEMBLED module (then push). DO NOT run this runbook as-is. See
> `research/findings/sync-consolidation-refactor-PLAN-2026-06-27.md` + [[project-sync-module-refactor-2026-06-27]].

---


> WHY NOW: D1 (steady-grab ghost-dup) and D2 (client-kerfur in-air flash) are STRUCTURALLY closed
> (`066d0a49` Element::IsSaveNative atomic flag; `df589591` adopt-by-eid) but NOT hands-on-verified.
> Variant-1 verify TRICE showed "correct by construction" hid a real bug (valve-skip / wrong-moment /
> gate-order). D1/D2 are the same status. The module assembly (next, days) RELOCATES these paths --
> so verify them HANDS-ON now, while the fix is fresh and un-moved, to catch any residual identity bug
> BEFORE it migrates. On PASS -> push the verified arc -> assembly on a verified base.

Deployed DLL: `2D0230013D35481A` (built at `8b85cb2e`; contains D1 `066d0a49` + D2 `df589591` + the
verified valve-abort/post-purge fixes). Host = `Game_0.9.0n`, Client = `Game_0.9.0n_copy`. Both MATCH
the build hash (verified). [dev] ini (both peers): `save_identity_bind=1`, `save_identity_map_log=1`,
`force_save_churn=0`. NO rebuild / redeploy needed.

## Steps (ONE session covers both)
1. HOST: launch `mp_host_game.bat`, load the recent fresh save (the standard 6-kerfur / chip-pile slot).
2. CLIENT: launch `mp_client_connect.bat`, join. Let the world finish loading (~15-25 s; the join
   reconcile fires at quiescence -- you'll have seen the clean census in past runs).
3. **D1 -- grab a SAVE-loaded chip-pile, STEADY STATE.** Wait ~1 minute after the world settles (so it's
   a steady-state grab, like the 15:01:49 repro -- NOT during the join window). Then, **on the CLIENT**
   (the joiner window), walk to a chip-pile that came from the save and GRAB it (carry it a moment).
   Watch: the pile should NOT leave a second copy / ghost behind. One pile, picked up cleanly.
4. **D2 -- toggle a CLIENT kerfur.** Still on the client, find a kerfur (kerfurOmega) and TOGGLE it
   (turn it off, or on -- the activation toggle). Watch: it should NOT flash a copy in mid-air that
   freezes then vanishes. The form should switch in place.
5. Quit both. Tell Claude "logs ready" -- Claude greps (you don't read logs).

(If a save-pile grab + a kerfur toggle in the SAME session is awkward, two short sessions are fine.)

## What Claude greps in the CLIENT log (PASS / FAIL)
**D1 (steady grab of a bound save-native):**
- PASS: `[PILE] CLIENT convert ... eid=N -> bound save-loaded NATIVE pile GRABBED -> handed to runtime
  clump proxy=... (native retired; native-authoritative hand-off)` -- printed ONLY when
  `morphBoundNative=TRUE` (the Element::IsSaveNative flag read correctly = the D1 fix). The native is
  retired -> no ghost.
- FAIL: a steady grab of a save-pile hitting the else-branch `proxy SPAWNED ... (convert beat its
  spawn)` with the native NOT retired, AND/OR a later `[PILE-CENSUS]` orphan / a ghost dup at that spot.

**D2 (client kerfur toggle):**
- PASS: `kerfur_convert[client]: adopted parked turn-off ghost as PROP mirror eid=N (by eid ...)`
  (toggle to prop/off) or `adopted parked turn-on ghost as NPC mirror eid=N (by eid)` (toggle to on).
  The "(by eid ...)" is the key -- adopted by the host's authoritative oldEid, no flash.
- FAIL: the parked ghost destroyed by the `CleanupParkedGhosts` timeout (no adopt) = the in-air flash,
  OR an adopt "(by position)" fallback (means the eid tag missed -- a partial fail worth noting).

## After
PASS on both -> the arc is FULLY verified (variant-1 by log + D1/D2 by hand) -> push the verified arc
to origin/main as one clean checkpoint -> start the module assembly on the verified base.
FAIL -> fix BEFORE push (never push a likely bug to origin); re-run.
