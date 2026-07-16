# The 24 big dishes — rotation/slew RE (2026-07-16)

Why: user 2026-07-16 — dish rotation is NOT synced ("пиры видят разное положение", worse after a
client catch starts them turning); wants per-dish logging. Sources:
`research/bp_reflection/{dish,dish_Child,coordRadarDish,ticker_disher,ticker_dishUncalib,mainGamemode,analogDScreenTest}.json`
+ `CXXHeaderDump/dish.hpp`. `@N` = uber byte offsets. All [MEASURED] static bytecode unless noted.
Companion: `votv-signal-chain-units-RE-2026-07-16.md` (the desk chain that commands the dishes).

## 1. Class roles

- **`Adish_C`** — the 24 big map dishes (building + in-dish serverBox + the slew machine).
- **`Adish_Child_C`** — one special dish: adds a distance/time-of-day LOD theater (spawns
  `NewBlueprint3_C` + toggles a `waterVolume` when `timeZ.X == 3`); rotation behavior inherited
  unchanged.
- **`AserverBox_dish_C`** — the server box deferred-spawned inside each dish building at
  BeginPlay (`type=1`, `dishIndex=Index`, key `DISHSERVER_<Key>`); `dish.server @0x3D0` points at
  it; its `isBroken` gates the desk's dish-status panel.
- **`AcoordRadarDish_C`** — the 3 small coordinate-tower radars (the desk cursor's `pingDishes`),
  NOT one of the 24.
- `alexDish` does NOT exist (zero hits in dumps) — was a red herring.
- Related tools: `tool_setDishCalibration` (toolgun, writes `calibration` by Index),
  `trigger_breakDish` (event trigger, breaks a dish's server by Index).

## 2. Identity (the logging key)

`Index` int32 @0x358 (level-authored) is canonical: the gamemode registers
`dishs[dish.Index] = dish` (`mainGamemode` @55696; `dishs @0x330`, `activeDishes @0x350` =
parallel bool array). Display name `nameDish @0x360` = `lib_C.getSatelliteName(Index)` — a fixed
25-entry NATO array starting at **Bravo** (0=Bravo .. 23=Yankee; 24=Zulu unused; no Alpha);
`techName @0x408` = lowercase. As an `Aactor_save_C` each dish also has `Key` FName @0x230.
=> per-dish log key: `Index` + `techName` (+ `Key`/loc per the identity-log rule).

## 3. The slew machine (fields @ dish.hpp)

| field | off | role |
|---|---|---|
| `lookAt` FVector | 0x378 | commanded ABSOLUTE target (param + ActorLocation, set in `startMovingTo`) |
| `isMoving` bool | 0x384 | whole-slew latch — **a moving dish IGNORES new `startMovingTo`** |
| `rot_A`/`rot_B` FRotator | 0x388/0x394 | rot_B = composed target rotator (rot_A unused) |
| `isRotate` bool / `Axis` int32 | 0x3A0/0x3A4 | per-phase flag; phase 0 = yaw (`axis_Z` comp), 1 = pitch (`axis_Y` comp) |
| `Speed` float | 0x3A8 | RInterpTo_Constant rate; ramped by Timeline_0 `Lerp(0.1, MaxSpeed, alpha)` @9011, starts 0.01 |
| `calibration` float | 0x3AC | precision 0..1 (status bar = 16 * getDir()^2 * calibration^2) |
| `maxSpeedRange` FVector2D / `MaxSpeed` float | 0x3B8/0x3C0 | **MaxSpeed = RandomFloatInRange(4.5, 5.5) rolled PER SLEW** @2994 |
| `calibLose` float | 0x3C4 | **RandomFloatInRange(0.5, 1.5) per slew** |
| `losePrec` bool | 0x3B4 | decay enable (CDO true; re-rolled 25% at slew end @3949) |
| `randrot` bool | 0x3C8 | CDO **true** — the load-time randomizer (below) |

**Interpolation**: a `Delay(0)` latent frame-loop (NOT ReceiveTick) while `isMoving`:
`RInterpTo_Constant(comp.RelativeRotation, phase part of rot_B, dt, speed)` ->
`K2_SetRelativeRotation` on `axis_Z` (yaw phase) then `axis_Y` (pitch phase); arrival =
rotator-equal tol 1.0; phase switch @1373 inserts `Delay(Random 1.5-3.0 s)`; slew end: axis 1
done -> `isMoving=false`, audio fade, `gamemode.activeDishes[i]=false` +
`gamemode.checkFordDishes()` (@3660). Inside the same loop @1525: if `losePrec`, per-frame
`calibration -= Ease(0, dt/5, RandomFloat(), ...) * calibLose` (clamp 0..1) — **per-frame
per-peer RNG decay**.

> CORRECTIONS 2026-07-16 (impl-gap pass, `votv-dish-impl-RE-2026-07-16.md`): (a) §5's tail was
> mis-attributed — `objectRenderer.begin()` + `signalFound()` + the camera aim run INLINE in
> `checkFordDishes` AFTER the broadcast (gamemode-side), NOT in the desk's dishesStop handler
> (which is `formDownload(0,-1)` only); (b) §6's "Solar event" is the desk's `virusEvent` chain
> (initiating peer only), `event_solar` is a trigger REFERENCE on trigger_eventer, not an event.

**Slew start** (`startMovingTo` -> `startMove` -> @2994): gate `isMoving`; roll MaxSpeed;
`activeDishes[Find(dishs, self)] = true`; roll calibLose; **`Delay(RandomFloatInRange(1.0,
12.0))`** (!) -> isRotate + `Delay(Random 2.0-4.0)` -> @2200 `rot_B = ComposeRotators(
MakeRotator(roll, Lerp(20, 90, pitch/90), yaw) of (lookAt - axis_Y.Location), const)` (pitch
remapped into 20-90 deg) -> frame loop. `movePow` (@6405) starts move audio + the speed-ramp
timeline. `stop()` just clears the flags. `getCoords()`: X = `axis_Z.RelativeRotation.Yaw`,
Y = `axis_Y.RelativeRotation.Roll`. `getDir()` = |dot(Arrow1.fwd, Arrow.fwd)| (aim vs boresight,
feeds the desk status UI).

## 4. THE DIVERGENCE ROOTS (ranked)

1. **BeginPlay randomization, per peer, NOT saved** (@8450 -> @6648): if `randrot` (default
   TRUE): `axis_Z` world yaw = `RandomFloatInRange(0, 360)`, `axis_Y` pitch =
   `RandomFloatInRange(90, 135)`. `getData`/`loadData` persist ONLY calibration/hashcode/
   serverKey/broken — **orientation never rides the save**, so host and joiner start with
   different rest poses BY CONSTRUCTION. This alone reproduces the user's report before any
   catch.
2. **Per-slew RNG**: MaxSpeed (4.5-5.5), start delay 1-12 s, phase delay 1.5-3 / 2-4 s,
   calibLose, losePrec 25% re-roll, per-frame calibration decay.
3. **The `isMoving` gate**: a dish already slewing locally (e.g. ticker_disher fired on this
   peer only) silently DROPS a new catch target -> permanent target divergence for that dish.
4. **Arrival bookkeeping is derived per peer**: the LAST-dish-arrival fires
   `checkFordDishes` -> `BROADCAST dishesStop` -> the desk's `formDownload(0,-1)` (download
   arming) — at each peer's own time.

## 5. Catch -> rotation chain

Desk uber successful-ping block @9337: `master_spaceRenderer.deleteSignal(element)` -> log
"Successful ping. Initializing satellite rotation..." -> **loop over `gamemode.dishs`: every
dish `startMovingTo(master_spaceRenderer.getCoords(normCoords()) * 100000 + const)`** — ONE
shared relative vector for all 24 (@9494-@9908); then DL_* resets. When the last dish stops:
`checkFordDishes` (@60831 mainGamemode) -> `dishesStop` broadcast -> desk arms the download +
`objectRenderer.begin()` + `spaceRenderer.signalFound()`. All computed on the pinging peer;
nothing native crosses peers.

**What already ships** (`coop/interactables/signal_catch_sync.cpp` + `ue_wrap/dish.h`): the
catch detector = rising edge of `MovingCount()` on the claim holder; `SkySignalCatchPayload`
carries the recovered slew vector (`lookAt - ActorLocation` of any moving dish); receivers
`StartMovingAll(slew)`. => **targets ARE synced at catch time; rest pose + kinematics/timing are
NOT.** (`DishAimState` 55 is the coords-PANEL cursor locks — rotates the 3 coordRadarDish
towers, not the 24.)

## 6. Ambient dish tickers (both per-peer RNG)

- **`ticker_disher`**: one-shot timer `RandomFloatInRange(1800, 3600)` s; on fire re-arm +
  `RandomBoolWithWeight(0.05)`; if won AND no dish active: every dish
  `startMovingTo(Normal((1,0,1)) * 100000 + const)` — target is a FIXED constant
  (deterministic), **timing is pure per-peer RNG** (fires on one peer, not the other).
- **`ticker_dishUncalib`**: tick interval `RandomFloatInRange(5, 10) * difficultyMult`
  (0.5..2.0); each tick `Array_Random(gamemode.dishs).losePrec = true` — **which dish decays is
  per-peer RNG** (measured live 2026-07-10, dispatch-visibility row 103).
- Solar event (@6150 desk uber): on `event_solar` every dish `calibration := RandomFloat()` —
  per-peer values.

## 7. Dispatch visibility

`startMovingTo` (desk @9908, ticker @386) and `startMove` = `EX_Context +
EX_LocalVirtualFunction` -> PE- and Func-patch-INVISIBLE (the shipped detector correctly POLLS
`isMoving`). All rotation-state writes = inline EX_Let/EX_CallMath natives -> invisible. The
`Delay(0)` latent resume + Timeline update funcs re-enter via ProcessEvent [RD]; timer/Delay ARM
natives are EX_CallMath (invisible, row 103). `ReceiveBeginPlay` PE-visible.

## 8. Convergence read-set (facts for the future design; NOT a design)

The complete observable pose = `axis_Z`.RelativeRotation (yaw) + `axis_Y`.RelativeRotation
(pitch/roll) — exactly what `getCoords()` reads and the loop writes. Full driver state:
`lookAt, isMoving, isRotate, Axis, Speed, MaxSpeed, rot_B` + precision trio
`calibration/losePrec/calibLose`. rot_B is re-derived from lookAt at @2200; arrival/
`checkFordDishes`/`dishesStop` are derived from pose-vs-rot_B. RNG lives only in
MaxSpeed/delays/calibLose/losePrec/decay. Orientation is not persisted — no save-side channel to
fight. Per-dish logging keys exist (§2).
