# Hands-on runbook — L1 orphan CENSUS (Phase 1, read-only) — 2026-06-23

**Deployed DLL SHA:** `5D4C9E1AA08DEA5E` (host + client folders confirmed).
**Proto version:** unchanged (no wire change — the census is client-local; the drift scenario
edits the host's own world). **HEAD:** pending commit. **Push:** HELD.

## What this build adds (all READ-ONLY / additive — nothing is removed yet)
1. **`[PILE-CENSUS]`** — at the client's join-reconcile sweep, after the world has quiesced, the
   client enumerates the native chipPiles that NO host proxy claimed within 1cm. These are the
   **host-drift orphans** (the host moved/collected a pile since the save the client loaded) — the
   exact set the existing sweep MISSES (a level-native enters the Prop Registry lazily, so the
   membership sweep never sees it). This is the root of L1: a human aims at one of these surviving
   natives, grabs it through the *real* interaction system (it's a real `actorChipPile_C`, not our
   proxy), so no GrabIntent is sent → the host never sees the grab. The census **only logs** them,
   banded by nearest-proxy distance — the data that picks the Phase-2 removal thresholds.
2. **Host-drift scenario** (`VOTVCOOP_RUN_PILE_DRIFT=1`, host-only) — automates the divergence: the
   host destroys 5 + moves 3 of its own piles in the pre-connect window so its snapshot diverges
   from the save, seeding known orphans for the census.
3. **`mp.py smoke --host-settle N`** — a host-solo window before the client launches (default 0).

**Nothing is destroyed on the client yet.** Phase 2 (the absence-removal) comes after we read this
histogram. Safe to run — worst case it logs nothing.

## The census line to read (CLIENT log)
`Game_0.9.0n_copy\WindowsNoEditor\VotV\Binaries\Win64\votv-coop.log`:
```
[PILE-CENSUS] N live orphan native(s) (K in index of M built): le5=A (near-miss dup) 5_30=B (ambiguous) gt30=C (true orphan/moved) noProxy=D (collected) -- ...
```
- `M built` = native chipPiles the client loaded; `N` = how many survived as orphans.
- `le5 / 5_30 / gt30` = orphans banded by distance to the nearest host pile proxy (cm).
- `noProxy` = no host pile anywhere near → a collected orphan.
This band split is exactly what I need to choose the Phase-2 absence-removal thresholds.

---

## Path A — AUTOMATED (recommended; one command you run, I read the logs)
Seeds a deterministic drift, joins, censuses, asserts — no manual pile-collecting.
```powershell
$env:VOTVCOOP_RUN_PILE_DRIFT = "1"
python tools/mp.py smoke --host-settle 40 --duration 200
```
- Host launches, runs solo 40 s; in that window the host log shows
  `[PILE-DRIFT] HOST destroyed pile #..` ×5, `[PILE-DRIFT] HOST moved pile #..` ×3, then
  `[PILE-DRIFT] HOST drift COMPLETE -- 5 destroyed + 3 moved = 8 orphans seeded`.
- Then the client launches + joins; its log shows `[PILE-CENSUS] ...` with the bands.
- After it finishes: `pwsh tools/pile-test-assert.ps1` →
  `pile-census-orphans` (INFO, the band split) + `pile-drift-censused` (WARN, must find >0).
- **Tell me the `[PILE-CENSUS]` line + the assert table.** That's the Phase-2 input.

Clean up the env afterward: `Remove-Item Env:\VOTVCOOP_RUN_PILE_DRIFT`.

## Path B — NATURAL HANDS-ON (most faithful; real drift you create)
If you'd rather see it on a real played-in world:
1. Launch the **host**, load a **fresh New Game** save.
2. As host, **collect ~5 chipPiles** (grab + carry them off / let them get collected) and optionally
   drag a couple to new spots — real host-drift. Do this BEFORE the client joins.
3. Launch the **client** → it joins.
4. Read the **client** log for `[PILE-CENSUS]` and send me the line.
(No env vars needed — the census summary line is always logged.)

## What I do with the result
The band split tells me whether the orphans land cleanly (`gt30` true orphans + `le5` near-miss
dups) or smear into the ambiguous `5_30` middle (dense-cluster overlap). I then build Phase 2: the
banded absence-removal (destroy the confident bands, watch the middle) with its own proportional
valve — **but first I must extract the pile-reconcile code out of `remote_prop_spawn.cpp`** (now
1496/1500 LOC — at the hard cap; Phase 2 can't be added there).
