# STOLAS UNIT CADDY (wallunit_tapes) + the daily tape task — RE (2026-07-16)

Why: user 2026-07-16 — the reel unit records with a visible Progress %, tapes must be removed and
drone-sent daily ("YOU MUST SEND BOTH TAPES EVERY DAY FOR DAILY TASK"); coop sync status unknown.
Sources: `research/bp_reflection/{wallunit_tapes,prop_reel,prop_reelbox,droneSellLocation,
daynightCycle,lib,analogDScreenTest}.json`, `CXXHeaderDump/{wallunit_tapes,saveSlot}.hpp`,
`_map_untitled_1.json`. `@N` = bytecode offsets in the named function. All [MEASURED] unless
noted. Companion: `votv-signal-chain-units-RE-2026-07-16.md`.

## 1. The recorder — a per-peer 1 Hz accumulator (COOP_WORLD_PROP_DIVERGENCE class)

`Awallunit_tapes_C`, ONE placed instance on the map (`wallunit_tapes_2`; the desk's
`tapeObject @0x1360` is a level-baked pointer to it). ReceiveTick (TickInterval **1.0 s**,
uber @2022): if `active`: `reelBig += dt / speed` and `reelSmall += dt / speed`
(VictoryFloatPlusEquals), each FClamp(0, 100).

- CDO: `speed = 30.0`, `active = true`, `reelBig = 1.0`. Rate = 1/speed %/s -> 100% in ~50 min
  stock, ~16.7 min with tape compression.
- **`speed` is written from the DESK**: `analogDScreenTest.updPhysMods @401-596`:
  `has_tapeCompression := physMods.Contains(byte 21)` ->
  `tapeObject.speed := SelectFloat(10.0, 30.0, has_tapeCompression)` — a raw cross-object write.
- State encoding: `reelBig @0x288` / `reelSmall @0x28C` DOUBLE as slot state — **-1.0 = slot
  empty**, >= 0 = reel present at that progress. `active @0x290`. `upd()` applies mesh/anim
  visibility + `SetActorTickEnabled(active)`.
- Hover: "Progress: N%" = **the AVERAGE `(reelBig + reelSmall) / 2`** (the user's 66.5%), shown
  only when both reels present and active.
- Divergence: no RNG, but a local accumulator on a keyed world actor with tick ON on every peer
  = the concreteBucket class (`docs/COOP_WORLD_PROP_DIVERGENCE.md`); plus `speed` diverges if
  desk physMods aren't mirrored; plus every interaction below is local-only.

## 2. Interactions

- **Toggle** (E on usebox, action 4): only if both reels present -> `active = !active; upd()`.
- **Remove reel** (E on a reel slot; REFUSED while `active`): deferred-spawn
  `prop_reel_big_C`/`prop_reel_small_C` + **`SetFloatPropertyByName(actor, 'progress',
  reelBig)` pre-Finish** + FinishSpawningActor + slot := -1.0 + `player.Hold Object(...)`
  (straight into hands) + `upd()`.
- **Insert** (two routes): `playerUsedOn` with a held reel (refused while active) -> `setBigReel/
  setSmallReel(holdObject)`; or THROW-IN via `reelbox_big/small` BeginOverlap delegates.
  `setBigReel`: cast `prop_reel_big_C`, only if slot empty -> **`reelBig := prop.progress;
  prop.K2_DestroyActor(); upd()`**.
- **Nothing ever resets progress to 0**: progress rides ON THE PROP (`prop_reel.Progress
  @0x364`, own hover), restored on re-insert, capped 100. Fresh reels come from the store.
- `prop_reelbox` = the shipping box: `reeltop` (big) / `reelBottom` (small) / `lid` (accepts a
  prop named `reelcase_1`); same copy-progress-and-destroy sentinel scheme.

## 3. The daily task (state = `saveSlot.taskNew`, NOT gamemode)

`Fstruct_taskNew` @0xCA8 in saveSlot.hpp; `reel_big @0x40`, `reel_small @0x44`.

- **Day rollover** (`daynightCycle` uber @5184): `createNewTask()` (fresh struct, reels := 0,
  signal requirements from `upg_processLvl` + difficulty, dish subset shuffled from
  `gamemode.dishs`) -> `lib.setTaskNew`: FIRST `processTask(old)` (grades yesterday, reward
  email, addPoints), THEN `taskNew := new` + requirements email; `dailyDelivery := false`.
- **Sending** (`droneSellLocation.sell`, when the drone arrives): iterates the sack's
  **struct_save SNAPSHOTS** (actors serialized before flight — sell reads saved data, not live
  actors). Class dispatch @17348-18576: `prop_reel_C`/`prop_reel_small_C` -> saved `mFloat[0]`
  (Progress) -> **`taskNew.reel_small/reel_big := FMax(current, sent)`**. A `prop_reelbox`
  contributes `mFloat[0]->reel_big`, `mFloat[1]->reel_small`. Check = item-class match + saved
  progress; no task id, no minimum threshold.
- **Grading** (`lib.processTask` @1729-2115/@10599): tapes = 2 of maxPoints; **reward =
  round(reel_big/100*25) + round(reel_small/100*25), paid ONLY if BOTH > 0**
  (`canRewardTapes = big>0 AND small>0`) — hence "must send both". Reply quality =
  floor(4*score/maxPoints).
- **Early completion** (`sell @8359-8943`): all sigs + all dishes + both reels > 0 ->
  `processTask` immediately + `taskNew.active := false`.

## 4. Save-transfer vs live (the coop gap)

Join save-transfer DOES carry: the wallunit's keyed row (`mFloat[reelBig, reelSmall]` +
`mBool[active]`; loadData calls `upd()`), the whole `saveSlot.taskNew`, `prop_reel.Progress`,
reelbox slots. Mutates LIVE with no lane: the 1 Hz accrual on BOTH peers independently;
eject/insert/toggle (local; the ejected-reel spawn + inserted-reel destroy DO cross the generic
Func seams — host_spawn_watcher / prop_lifecycle — so the PROP side has lanes, the UNIT FIELDS
have none); `speed` via desk updPhysMods; `taskNew.reel_*` writes in sell/processTask/rollover.

## 5. Dispatch visibility

| Mutation | Mechanism | Visible? |
|---|---|---|
| reel accrual | ReceiveTick thunk -> uber; VictoryFloatPlusEquals inside | **ReceiveTick PE-VISIBLE (1 Hz)**; the writes invisible (VictoryFloatPlusEquals = Func-visible choke) |
| eject/insert/toggle entries | EX_LocalVirtualFunction from mainPlayer | INVISIBLE — poll state |
| throw-in overlaps | multicast delegate | VISIBLE |
| ejected spawn / inserted destroy | FinishSpawningActor / K2_DestroyActor | Func-VISIBLE (existing generic seams) |
| `lib.processTask`, `taskNew.reel_*`, `tapeObject.speed` | EX_Local* / raw EX_Let | INVISIBLE — poll |
