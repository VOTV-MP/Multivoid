# Hands-on runbook -- PROVE the Phase 0 floor on a real WIPE (2026-06-25)

**Deployed: floor-on-baseline MD5 `68633342` (proto v88, NO pile-09) on all 4 peers.**

## Why this runbook
The 13:21 run held its piles, but the FLOOR DID NOT FIRE -- the claim sweep saw only `88 in-universe,
88 claimed, 0 destroyed`, so `doomed` was empty and the floor (`!doomed.empty()`) was skipped. The
piles were safe because the client's native piles were NOT seeded as Prop Elements that run (the 11:16
non-determinism: 11:14 clean / 11:16 wipe). "Catastrophe didn't recur" != "the floor caught it." To
mark Phase 0 VERIFIED we must SEE the floor fire on a real wipe condition:
`remote_prop_spawn: completeness FLOOR kept K unclaimed 'actorChipPile_C'` WITH the piles surviving.

The floor only engages when, at sweep time, the client's native chipPiles are BOTH (a) seeded into the
Prop registry (so they are "in-universe" -- `claim sweep -- N in-universe` with N in the HUNDREDS/
THOUSANDS, not 88) AND (b) UNCLAIMED (the host under-expressed them). So the goal is to get a sweep
that DOOMS hundreds of piles -> the floor must then KEEP them.

## PATH A -- organic repro (the 11:16 cookbook, multiple cycles)
The wipe is non-deterministic; aim at it and repeat:
1. Host: fresh recent save with the FULL ~870 pile field (the more piles, the more likely they seed
   before the sweep). Piles near several kerfurs.
2. IN THE JOIN WINDOW (right as the client connects), do the 11:16 churn aggressively:
   - throw multiple piles near the kerfurs, AND
   - toggle several of those kerfurs off/on,
   so the host's pile expression is disrupted (the under-express that leaves the client's natives
   unclaimed).
3. Connect. Let the client world FULLY settle (wait the full load-tail; the seed-walk must seed the
   natives before the sweep fires -- do not rush).
4. Reconnect and repeat 5-10 cycles (each connect = `mp_client_connect.bat`; the wipe condition is a
   race, so more cycles = more chances).

WATCH (client log, say "ready" -> I grep):
- `remote_prop_spawn: claim sweep -- N in-universe ...` with **N in the hundreds/thousands** (NOT 88) =
  the natives ARE seeded this run -> the wipe condition is live.
- `remote_prop_spawn: completeness FLOOR kept K unclaimed 'actorChipPile_C' -- host census X, claimed
  only C` = **THE FLOOR FIRED** (this is the proof line).
- Piles SURVIVE on screen. -> Phase 0 VERIFIED.
- If a cycle shows `N in-universe` small (like 88) and `0 destroyed`: that was a CLEAN run (no wipe
  condition); the floor correctly stayed dormant -> RETRY.

## PATH B -- deterministic proof (recommended; I build a dev-only forcing flag on your OK)
Hoping a catastrophe recurs is a weak way to prove a catastrophe guard. The sure path is to INJECT the
wipe condition deterministically with a dev-only, ini-gated test flag (RULE-2-exempt probe, ships in no
release). Two candidate forcings (I'd build ONE):
- `[dev] force_chippile_unclaim=1` on the HOST -> the host SKIPS expressing chipPiles for one join
  (simulating the 11:16 under-express). The client then has its full ~870 natives seeded + unclaimed ->
  the sweep DOOMS them -> the floor MUST KEEP them. Deterministic: every run shows
  `completeness FLOOR kept ~870` + piles survive. WITHOUT the floor (old binary) the same flag would
  wipe all 870 -> a clean before/after proof.
- (alt) a client-side `[dev] force_seed_natives_before_sweep=1` to guarantee (a) the seeding race.
This proves the floor fires AND that it fires on the exact mass-unclaim shape, in one controlled run --
no waiting on non-determinism. Say the word and I build the forcing flag (small, dev-only).

## Acceptance (EITHER path)
- The line `completeness FLOOR kept K unclaimed 'actorChipPile_C'` appears (K large) AND the piles
  survive on the client. THEN Phase 0 floor is VERIFIED (seen firing on a real wipe).
- Until that line is seen on a doomed-non-empty sweep, Phase 0 stays UNVERIFIED (built + audited only).

## NOTE
- Your hands-on; I do NOT smoke while you play. pile-09 is NOT in this binary (clean attribution).
- The kerfur "missing on grab" you saw is SEPARATE (untracked-at-join off-kerfur, keyless-identity --
  Phase 1 territory, not the floor). See the Phase 1 design.
