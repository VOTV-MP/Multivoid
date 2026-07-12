# Hands-on runbook — 2026-07-12, TAKE 6 (the JOIN BARRIER — architecture, per rule 1)

Deployed DLL: **`7030160D`** (md5 first 8), all 4 installs hash-verified. Commits `bbf91f39` (the barrier) + `7847021e` (audit-CRITICAL fix: the off-GT Arm race -- probe session now opens on the GT via an atomic request).
SUPERSEDES take 5 (`0DA4FAE0`) BEFORE it ran: the user mandated the architectural fix of the
two-authority join seam with fresh context; take-5's mechanisms (wire-order netting, spawn
revalidation) are now RETIRED — the barrier makes their bug class structurally unreachable.
Design doc: research/findings/join-identity/votv-join-barrier-DESIGN-2026-07-12.md.

## What this build IS

The MTA join barrier (INITIAL_DATA_STREAM shape): the client announces `ClientWorldReady` only
when its loadObjects tail has SETTLED (the same population-stability probe the doom sweep always
trusted, now owned by `coop/session/world_load_episode`). The host therefore streams the ENTIRE
world state (R2 deletes -> snapshot -> state lanes -> teleport) into a settled world. No wire prop
expression is ever provisional; wire events apply strictly in arrival order. Roots 1/2/4/5 of the
placed-prop saga die structurally; roots 3+6 (duplicate save keys) stay fixed by the take-4
KEY-UNIQUENESS re-key authority (unchanged in this build).

User-visible deltas (expected, by design):
- The joiner stays under the loading cover ~5-20 s LONGER (the world genuinely settling; the
  reveal now lands in a FINISHED world instead of mid-churn).
- The joiner receives NO live host events during its load; the post-settle snapshot carries their
  net effect.

## Test steps (the take-2/4 repro IS the acceptance test)

1. Host: load the usual save ("Host game"). Client: connect; WHILE the client sits in the connect /
   world-load window, host hold-R a world rock into the hotbar and place it back somewhere visible.
2. After the client is in: the placed rock EXISTS on the client, at the host's placement spot.
3. Take-2 regression: a rock from pre-connect hotbar stock placed in-window also appears.
4. Join regression sweep: props/piles/kerfurs present, mushrooms/garbage not doubled.
5. fps sanity post-join (the take-3 2.5fps class).
6. Bare-join host-wipe regression: host keyed-prop census stable across a client join (episode
   destroy-broadcast suppression unchanged but now self-closing).
7. If convenient: cave in/out on the client (travel re-announce waits for the new world's settle —
   host re-replay lands post-settle; nothing should vanish/double).

## What to read in the logs

CLIENT (the load-bearing sequence, in this order):
- `world_load_episode: ARMED ...` then `world_load_episode: quiescence probe ARMED (join world-load)`
- `world_load_episode: load-tail QUIESCED (population stable; session 'join world-load') -- episode
  CLOSED ...` — the barrier edge. A `DEGRADED` variant here = the deadline fired (pathological
  load; the settled-world guarantee did not hold — report it).
- `net_pump: ClientWorldReady announced (world up + registry coherent + load tail quiesced)` —
  STRICTLY AFTER the QUIESCED line, and no PropSpawn/PropDestroy receive lines before it.
- Then the snapshot bracket + `join_membership_sweep: divergence sweep FIRING (probe latched ...)`.
- NO `[SPAWN-DEFER]` / `SUPERSEDED` lines anywhere (that machinery is deleted); NO dead-row
  tripwire for the rock's key.

HOST:
- `[PILE-1C] slot N world-ready -- JOIN-WINDOW CLOSED` arrives LATER than before (at the client's
  settle) — expected.
- First load of a poisoned save: the `KEY-UNIQUENESS ... re-keyed -> 'rk_...'` burst (root-6 fix
  working; `re-key FAILED` lines = regression, stop and report). ~0 after the game re-saves.

## Honest status

- Built + linked clean; deployed `07C189C4` x4 hash-verified; audit agent verdict: see the session
  report (launched on `bbf91f39`).
- NOT yet smoked (user was at the PC; autonomous LAN smoke needs the green light) and NOT
  hands-on verified. The take-6 hands-on above is the acceptance test.
