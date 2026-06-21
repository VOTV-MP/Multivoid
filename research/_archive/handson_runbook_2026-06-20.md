# Hands-on runbook -- 2026-06-20 (take 20): the pile MORPH V2 (carry + re-pile)

Deployed `votv-coop.dll` = **`6bb5ad4c612b770d`** (Release, proto **v81**) on HOST + CLIENT + CLIENT2 +
DEV (SHA256 verified MATCH ×4). **Restart any peer that was on the old build.**

## What changed since take-19 (the working bind)
The take-19 build was the take-17 adopt-BIND (piles mirror; host-grab -> client pile vanishes; the
carry/re-pile was the OPEN GAP). **v81 adds the MORPH** -- the grab->carry->throw->land sync, done right
this time:
- GRAB: the morphed clump is adopted onto the grabbed pile's eid E on the **PROVEN held-object channel**
  (NOT the dead Init-POST observer the take-18 attempt bet on). `PropConvert{ToClump}` re-skins the pile
  into a clump on every peer.
- CARRY: the held clump streams under E (the existing held-pose stream).
- THROW/LAND: a death-watch poll finds the re-piled pile -> `PropConvert{ToPile}` (re-seed-race-safe).
- FALLBACK: if the morph can't sync, a 400 ms deferred `PropDestroy(E)` still vanishes the mirror =
  the working take-17 grab->vanish. **A missed morph never regresses the grab.**

Design + as-built: `docs/piles/07-MORPH-V2-held-object-channel.md`. Cross-cutting truth (new this
session): `docs/COOP_ENTITY_EXPRESSION_MAP.md` + `docs/COOP_DISPATCH_VISIBILITY.md`.

## Status (be honest) -- UPDATED 2026-06-20 (session 34): VERIFIED autonomously
The morph's full grab->carry->throw->land cycle is now **runtime-verified end-to-end, cross-peer**, by a
NEW autonomous harness test (`harness/autotest_chippile.cpp`, env `VOTVCOOP_RUN_CHIPPILE_TEST=1`) that
fires the pile's OWN `playerGrabbed` (the genuine conversion) + probes the result -- see docs/piles/07
"Verification". Empirical correction: the grabbed clump rides **`grabbing_actor`** (the PHC path), NOT
`holding_actor` (the old assumption); the morph adopts it fine because local_streams reads grabbing_actor
first. **Your hands-on grab is now OPTIONAL** -- only to confirm the on-SCREEN visual (clump visibly in the
host-puppet's hand on the client; correct pile variant at the right spot). The sync plumbing is verified.

## The test (New Game on a FRESH save, both peers on v81)
1. **Host grabs a pile** (walk up, press E). Expect: on the CLIENT you SEE the round clump in the
   host-puppet's hand (carried), not just the pile vanishing.
2. **Host throws** (LMB). Expect: both peers see it re-pile (correct variant) at the same spot.
3. **Client grabs / throws** -> the HOST mirrors it.
4. **Spam-grab E** on piles -> NO local-clump dupe accumulation, NO pile dupes.
5. FPS normal.

## What to read in the log (host + client votv-coop.log) -- the morph instruments every edge
- `trash_collect: pile grab (E-press ...)` then `pile_morph: grab armed -- eid=N`
- `[probe garbage_pickup]: ... holding_actor=0x... (prop_garbageClump_...)`  <- THE measurement: is the
  clump in holding_actor? (if grabbing_actor instead, or null, that's the diagnosis)
- `pile_morph: ADOPTED held clump ... -> PropConvert{ToClump}; land-watch armed`  <- the grab synced
- `remote_prop::OnConvert: eid=N re-skin -> clump`  (on the OTHER peer) <- the carry mirrored
- `pile_morph: land detected for eid=N -> ... PropConvert{ToPile}`  <- the re-pile synced
- `pile_morph: deferred-destroy fired -- eid=N not adopted in 400ms`  <- the FALLBACK (morph missed,
  grab still vanished the mirror = take-17, no regression). If you see THIS instead of ADOPTED, the
  clump did not reach holding_actor in time -> read the `[probe garbage_pickup]` line for why.

## If it doesn't work
It will NOT dupe or regress the grab (the fallback guarantees that). If the carry/re-pile doesn't sync,
the `[probe garbage_pickup]` + the `pile_morph:` lines tell us exactly which edge failed -- send the
log tail. The likely culprit if it fails: the clump lands in `grabbing_actor` not `holding_actor`, or a
timing gap -> a one-line fix in `local_streams.cpp` (the read) or the 400 ms window.

## Autonomous prep done (Claude, user away)
- Built Release `6bb5ad4c` (the deploy sources Release, NOT RelWithDebInfo -- caught by SHA-verify).
- Deployed ×4 (SHA MATCH). `garbage_pickup_probe=1` enabled in the 3 hands-on inis.
- Ran the joinchurn LAN smoke (regression check) -- see `research/joinchurn_v81_morph.out`.
- Did NOT exercise a real pile grab (the chipPile-interaction smoke is unbuilt; the harness grab_test is
  a PHC heavy-prop grab, not the morph path). The morph FEATURE needs your hands-on grab.
- NEVER claim fixed from a smoke -- only your hands-on (or a matching real log) is the verdict.
