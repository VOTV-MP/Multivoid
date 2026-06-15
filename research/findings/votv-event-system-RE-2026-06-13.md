# VOTV event system — reverse engineering for the F1 dev "Events" menu (2026-06-13)

READ-ONLY RE. Goal: let the HOST trigger ANY game event via a reflected call, to dev-test
coop sync/mirroring. This doc is the engine ground truth: registry shape, the 100% event
list, the exact trigger verb(s), the scheduler, per-event notes, and safety flags.

Sources: `research/pak_re/extracted/VotV/Content/` (cooked assets), `research/bp_reflection/`
(kismet dumps). Bytecode addresses are `@NNNN` offsets in the named ubergraph/function.

---

## 0. TL;DR model (how VOTV events actually work)

- The **event REGISTRY** is the DataTable `/Game/main/datatables/list_events`
  (RowStruct = `struct_event`), **69 rows**. Each row = `time` (IntVector) + `specialTrigger`
  (FName). The DataTable is only the *day/time schedule catalog*.
- The **dispatch SUPERSET** is one giant `SwitchName` inside
  `trigger_eventer.runEvent` — **65 distinct event names** + a `specialTrigger=='ariralPrank'`
  branch. 53 of the 65 are also DataTable rows; **12 are switch-only** (story / location /
  trigger-driven, not randomly scheduled); the remaining 16 DataTable rows are the
  `arirInteraction_*` rows that all funnel into the single `ariralPrank` branch.
- **THE TRIGGER VERB (this is what the menu calls):**
  `lib.getMainGamemode().eventer.runEvent(FName event, FName special)`
  where `eventer` is the single placed `trigger_eventer_C` actor the gamemode caches at
  BeginPlay. Two FName params, `FUNC_BlueprintCallable|FUNC_BlueprintEvent`,
  cross-object → **PE-visible and ParamFrame-callable**. This is exactly how the game's own
  in-game debug widget `ui_eventRun` runs events.
- `lib.setEvent`/`getEvent`/`isEventActive` are a **refcount** (`gamemode.activeEvents` int +
  `activeEvents_senders` array), NOT a by-name trigger. They are the "an event is active" gate
  the sleep/nightmare code reads. There is **no `stopEvent(name)`** — each event self-terminates.

---

## 1. THE EVENT REGISTRY

### 1.1 The DataTable
- Asset: `/Game/main/datatables/list_events` (`UDataTable`, RowStruct `struct_event`).
- `struct_event` has exactly two fields (mangled GUID names in the dump):
  - `time` — `FIntVector` `(X, Y, Z)`.
    - **`Z` = unlock DAY** (confirmed: `mainGamemode @41631` filters `startDay > row.time.Z`
      to build `passEvents`; `daynightCycle @8840` checks `savedtime.Z == lastRow.time.Z` for
      the `alphaFinish` achievement). Z rises monotonically down the table (1 → 47).
    - **`X`, `Y` = presumed (startHour, endHour) window**, BUT `time.X`/`time.Y` are read by
      **NO dumped BP** (grep over mainGamemode + daynightCycle = 0 hits). Either the hour-window
      picker lives in an undumped asset or X/Y are vestigial. See UNKNOWN-A.
  - `specialTrigger` — FName. `None` for normal rows; `ariralPrank` for the 16
    `arirInteraction_*` rows.
- Row enumeration recipe: `GetDataTableRowNames(list_events)` →
  `GetDataTableRowFromName(list_events, name)` (used at `daynightCycle @8615/@8761`,
  `mainGamemode @41089/@41552`, `ui_eventRun @47`).

### 1.2 The dispatch superset (the real "what can be triggered" list)
`trigger_eventer.runEvent(event, special)` → `ExecuteUbergraph_trigger_eventer(528)` →
`SwitchName(event)` over **65 names**. Per-case action and category below. Legend:
- **sub** = `event_<X>.runTrigger(self,0)` (calls a pre-owned sub-event object reference)
- **spawn** = `BeginDeferredActorSpawnFromClass(<Class_C>)` + `FinishSpawningActor` (fresh world actor)
- **fx** = inline particle/visual; **force** = queues name into `saveSlot.forceObjects`;
  **player** = moves/affects the player directly

| # | event name | in DT? | dayZ | mechanism (@target) | spawned/called | category |
|---|---|---|---|---|---|---|
| 1 | arirShip | Y | 12 | sub @4390 | event_arirShip.runTrigger | ariral/story |
| 2 | solar | Y | 3 | sub @4433 | event_solar.runTrigger | sky/visual |
| 3 | falseEnter | **N** | – | sub @4477 | event_falseEnter.runTrigger | scare |
| 4 | vehtp | **N** | – | sub @4521 | event_vehicletp.runTrigger | world |
| 5 | agrav | Y | 16 | sub @4565 (gate: lib.isPhysicalEvents) | event_agrav.runTrigger | physics |
| 6 | arirSignal | Y | 8 | sub @4665 | event_arirSignal.runTrigger | signal |
| 7 | starRain | Y | 1 | fx @4709 | emitter `eff_shootingStar_rain` | sky/visual |
| 8 | call0 | Y | 47 | sub @4783 | event_bigmRoar.runTrigger | story (end) |
| 9 | mann | **N** | – | sub @4827 | event_mann.runTrigger | scare |
| 10 | picSignal | Y | 11 | sub @4871 | event_arirpicSignal.runTrigger | signal |
| 11 | arirSpk | Y | 10 | sub @4915 | event_arirSpk.runTrigger | ariral |
| 12 | peace | Y | 5 | sub @4959 | event_peace.runTrigger | ambient |
| 13 | wisps | Y | 13 | sub @5003 | event_wisps.runTrigger | visual |
| 14 | arirFollower | **N** | – | sub @5047 | event_arirFollower.runTrigger | ariral NPC |
| 15 | arirEgg | **N** | – | sub @5091 | event_arirEgg.runTrigger | ariral |
| 16 | vent | **N** | – | sub @5135 | event_vent.runTrigger | scare |
| 17 | crys | **N** | – | sub @5179 | event_crys.runTrigger | world |
| 18 | fakeGrays | **N** | – | sub @5223 | event_fakeGrays.runTrigger | scare |
| 19 | treehouse_0 | Y | 16 | sub @5267 | event_treehouse_0.runTrigger | story build |
| 20 | treehouse_1 | Y | 17 | sub @5311 | event_treehouse_build_1.runTrigger | story build |
| 21 | treehouse_2 | Y | 18 | sub @5355 | event_treehouse_build_2.runTrigger | story build |
| 22 | treehouse_3 | Y | 19 | sub @5399 | event_treehouse_build_3.runTrigger | story build |
| 23 | treehouse_4 | Y | 20 | sub @5443 | event_treehouse_build_4.runTrigger | story build |
| 24 | treehouse_5 | Y | 21 | sub @5487 | event_treehouse_build_5.runTrigger | story build |
| 25 | break_Victor | Y | 16 | sub @5531 | event_breakVictor.runTrigger | story |
| 26 | break_Victor2 | Y | 19 | sub @5531 | event_breakVictor.runTrigger | story |
| 27 | break_RomeoSierra | Y | 7 | sub @5575 | event_break_RomeoSierra.runTrigger | story |
| 28 | picnic | Y | 7 | sub @5619 | event_picnic.runTrigger | world prop |
| 29 | destroyPicnic | Y | 9 | sub @5662 | event_destoyPicnic.runTrigger | world prop |
| 30 | toeStab | Y | 23 | sub @5706 | event_toeStab.runTrigger | scare |
| 31 | cookier | Y | 24 | sub @5806 | event_cookier.runTrigger | npc |
| 32 | susArir | **N** | – | sub @5852 | event_susArir.runTrigger | ariral |
| 33 | arirSat_0 | Y | 25 | sub @5952 | event_arirSat_0.runTrigger | signal/sat |
| 34 | arirSat_1 | Y | 25 | sub @5996 | event_arirSat_1.runTrigger | signal/sat |
| 35 | arirSat_2 | Y | 25 | sub @6040 | event_arirSat_2.runTrigger | signal/sat |
| 36 | bedEvent | **N** | – | sub @6084 | event_bedEvent.runTrigger | sleep/dream |
| 37 | earthTp | Y | 26 | **player** @6128 | `daynightCycle.skysphere.tp()` | **TELEPORT** |
| 38 | paperGray | **N** | – | sub @6255 | event_paperGray.runTrigger | prop |
| 39 | obelisk | Y | 24 | sub @6299 | event_obelisk.runTrigger | story struct |
| 40 | piramid | Y | 30 | sub @6343 | event_piramid.runTrigger | story struct |
| 41 | piramid_sig | Y | 29 | sub @6387 | event_piramid_sig.runTrigger | signal |
| 42 | tentacleBalls | Y | 32 | spawn @6431 | tentacleBallsFollower_C | creature |
| 43 | looker_0-1 | Y | 27 | force @6617 | forceObjects += looker_sky | looker NPC |
| 44 | looker_1-1 | Y | 27 | force @6761 | forceObjects += looker_window | looker NPC |
| 45 | looker_2-1 | Y | 28 | force @6905 | forceObjects += looker_* | looker NPC |
| 46 | looker_3-1 | Y | 28 | force @7049 | forceObjects += looker_* | looker NPC |
| 47 | looker_4-1 | Y | 28 | force @7193 | forceObjects += looker_* | looker NPC |
| 48 | soltoClean | Y | 34 | spawn @7337 | soltomiaCleaning_C | ariral NPC |
| 49 | morningGay | Y | 36 | spawn @7620 | morningUfo_C | UFO |
| 50 | borgRozital | Y | 37 | spawn @7835 | rozitBorg_C | story NPC |
| 51 | rozitalHole | Y | 38 | spawn @8050 | event_bottomHoleController_C | world |
| 52 | ventCrawler | Y | 4 | spawn @8265 | ventCrawler_C | creature |
| 53 | ventKnocker | Y | 11 | spawn @8480 | kocker_C | scare |
| 54 | fallbody_0 | Y | 39 | spawn @8695 | ufoDropper_body_C | UFO drop |
| 55 | fallbody_1 | Y | 40 | spawn @8910 | ufoDropper_body_C | UFO drop |
| 56 | fallcar_0 | Y | 41 | spawn @9125 | ufoDropper_car_C | UFO drop |
| 57 | enasus | Y | 14 | spawn @9340 | prop_notebook_paper_enasus_C, prop_snack_sushi_C | prop drop |
| 58 | enacros | Y | 19 | spawn @10086 | prop_notebook_paper_enacros_C, prop_food_croissant_C | prop drop |
| 59 | graysforest | Y | 42 | spawn @10868 | grayEventController_C | invasion |
| 60 | graystank | Y | 43 | spawn @10935 | ufoDropper_tank_C, ufoDropper_pig_C | invasion |
| 61 | arirBuster | Y | 14 | spawn @11379 | arirBusterSpawner_C | ariral |
| 62 | salt | Y | 24 | spawn @11594 | saltpile_C (+ `.dropHeart()`) | prop |
| 63 | eggvasion | **N** | – | spawn @11845 | superEgger_C | invasion |
| 64 | boarwar | Y | 46 | spawn @12060 | boarInvasion_C | invasion |
| 65 | dreambase | Y | 30 | spawn @12525 | dreamer_dreambase_C, eventSpawnerPivot_C, ariralThrowStuff_C | dream/story |

**12 switch-only names (NOT in the DataTable random pool — story/trigger-driven):**
`falseEnter, vehtp, mann, arirFollower, arirEgg, vent, crys, fakeGrays, susArir, bedEvent,
paperGray, eggvasion`. These are still fully `runEvent`-triggerable.

**The 16 `arirInteraction_*` DataTable rows** (specialTrigger=`ariralPrank`; dayZ in parens):
`_0(8) _1(13) _2(15) _3(16) _4(17) _5(18) _6(19) _7(20) _8(21) _9(22) _10(24) _11(33) _12(35)
_13(31) _14(26) _15(28)`. All 16 ignore the row name and run the **same** rep-gated random
ariral interaction (see §2.4). For the menu they collapse to ONE entry: "ariralPrank".

### 1.3 The cooked `event_*_C` BP class family (12 assets, `objects/`)
`event_arirFuelsAtv` (+`_toolbox`), `event_arirStealsGun`, `event_arirTreehouseSleep`,
`event_bottomHoleController`, `event_consoleWrite`, `event_fleshRain`, `event_funnyGascans`,
`event_lightsTurnoffer`, `event_passwordGuesser`, `event_trashPiles`, `event_vaccine`,
+ `misc/event_fossilBoarWar`. These are individual event implementations. Some are referenced
as the `event_<X>` sub-object instance vars on `trigger_eventer` (the `sub` rows above), some
are spawned by class. They are NOT a separate registry — `runEvent` is the only entry that
ties names to them.

---

## 2. THE TRIGGER PATH

### 2.1 The verb the menu must call
```
gamemode = lib.getMainGamemode()              // our existing resolver
eventer  = gamemode.eventer                    // UObject*, a trigger_eventer_C (set @ mainGamemode @39958)
eventer.runEvent(FName event, FName special)   // <-- THE trigger
```
- `eventer` is cached once: `mainGamemode @39958: eventer := GetActorOfClass(trigger_eventer_C)`.
  There is exactly one placed `trigger_eventer` in the level.
- `trigger_eventer.runEvent` flags: `FUNC_BlueprintCallable | FUNC_BlueprintEvent`.
- Param frame (from reflection): **two FName** —
  `@0x00 event` (size 0x0c), `@0x10 special` (size 0x0c), **total frame 0x20**.
  Note FName is **12 bytes (case-preserving)** in this build — same casing model behind the
  known FNAME-casing bug; reuse our existing case-correct FName path.
- Body (`runEvent` → uber @528):
  `@528: if special != 'None' → @4225` (special branch, §2.4);
  else giant `SwitchName(event)` → the per-case action in the §1.2 table; default falls through
  to `RETURN`. So an unknown name is a safe no-op.

### 2.2 PE-visibility / callability verdict
`runEvent` is invoked cross-object (`EX_Context` on `gamemode.eventer`) with no out-params and
only FName ins → it travels through `ProcessEvent`, so our detour both *sees* it and can *call*
it via `ProcessEvent(eventer, runEvent_UFunc, &frame)`. **No TArray/struct params — fully
ParamFrame-compatible.** This is the cleanest possible trigger for the menu.

### 2.3 The "active event" refcount (NOT a trigger; the gate other systems read)
- `lib.setEvent(bool isActive, /*+worldctx*/)` (`lib.setEvent`, 147 stmts):
  - isActive=true → `gamemode.activeEvents += 1`; `activeEvents_senders.Add(worldContext)`;
    logs to metaPLog; if `deactivateAmbientTrack` also `disableEventTrack += 1`.
  - isActive=false → the inverse (decrement). Event actors call this themselves on begin/end.
- `lib.getEvent()` returns `isEventActive = (gamemode.activeEvents > 0) OR (player NOT in box)`.
  This is the "an event is active" sleep/nightmare gate
  (`votv-sleep-nightmare-RE-2026-06-12.md`).
- `lib.isEventActive`, `lib.disableEventTrack` — related query/track helpers.
- **STOP/END path:** there is no `clearEvent(name)`. Each spawned actor / `runTrigger`
  sub-object owns its lifetime and calls `setEvent(false)` when it finishes (or is destroyed).
  For the menu: triggering is fire-and-forget; to "end" an event you let it finish or destroy
  the spawned actor it produced.

### 2.4 The ariralPrank special branch
`runEvent` `@4225: if special=='ariralPrank' → summonArirPrank()`.
`summonArirPrank` (`@28: gamemode.ariralRepHandler.calcRep()` → 5-case rep switch →
`Array_Random` over a reputation-tiered prank-option array → throws via the
`ariralThrowers_good` actors gathered in `trigger_eventer` BeginPlay). So the prank picked
depends on current ariral reputation; invoke as `runEvent(<anything>, 'ariralPrank')`.

---

## 3. THE SCHEDULER (random rolls + day/time gating)

### 3.1 What IS in the dumps
- **Load-time day-gate filter** (`mainGamemode @41089`, runs when `startDay > 0`):
  `saveSlot.allEvents := GetDataTableRowNames(list_events)`; then for each row, if
  `startDay > row.time.Z` → `saveSlot.passEvents.Add(name)`. So `passEvents` = every event whose
  unlock-day has already passed; `allEvents` = the full name cache. Both persist on the save.
- **Alpha-finish gate** (`daynightCycle @8454-9011`): if current day `savedtime.Z` equals the
  **last** `list_events` row's `time.Z` (=47, `call0`) → `progressAchievement('alphaFinish')`.
- **Per-event self-registration**: spawned/`runTrigger` actors call `lib.setEvent(true/false)`.

### 3.2 What is NOT in the dumps (the actual nightly roller) — UNKNOWN-A
- **No central "pick a random `passEvents` row and `runEvent` it" function exists in any dumped
  BP.** Evidence: only 3 assets reference `runEvent` at all — `trigger_eventer` (defines it),
  `ui_eventRun` (the dev widget calls it), `saveSlot` (data). `passEvents` has a writer
  (mainGamemode) but **no consumer** in the dumps. `time.X`/`time.Y` (the hour window) are read
  nowhere. Named-function lists for `mainGamemode`/`daynightCycle` contain no
  `roll*/tryEvent*/scheduleEvent*` verb.
- Interpretation: in this build, random events are driven by **placed level triggers**
  (`trigger_eventer`/`trigger_eventt_*`/`trigger_bedEvent` → `processKeys`/`loadTriggerData`) and
  story progression, with `runEvent` as the universal entry; the ambient weather/sky events have
  their own dedicated gamemode/daynight functions + timers (next bullet). The hour-window picker,
  if it exists, is in an asset not in `research/bp_reflection/`.
- **Suppression knob for clients (when we get there):** because `runEvent` is the single choke
  point and `eventer` is one actor, the clean client-side suppression is **"clients never call
  `runEvent`; host is authoritative and mirrors the resulting actors/state"** (same shape as the
  U6/nightmare precedent). The load-time `passEvents` build is host save-state and can stay
  host-only. No engine bypass flag is needed.

### 3.3 Separate ambient event spawners (own functions, not the runEvent table)
`mainGamemode`: `trySpawnInsomniac`, `spawnBlackFog`, `spawnRedSky`, `Spawn Bad Sun`,
`flowerSpawner`, `spawnErrorObject`. `daynightCycle`: `spawnFog`, `fogEvent`, `superFogEvent`,
`timerLightning`, `timerRain`, `causeRain`, `setFogDensity`. These are the weather/sky "events"
(fog, power, red sky, bad sun) the user mentioned; they are triggered by daynight timers/delegates
independently of `runEvent`. If the menu wants fog/redSky/badSun too, these are **separate
reflected calls** (e.g. `gamemode.spawnRedSky()`, `gamemode.spawnBlackFog()`,
`daynightCycle.superFogEvent()`) — see UNKNOWN-B for their signatures.

---

## 4. PER-EVENT NOTES (mirroring guidance)

- **Spawn-actor events (mirrorable via our prop/NPC pipelines):** rows 42, 48-65 in §1.2 —
  `tentacleBalls, soltoClean, morningGay, borgRozital, rozitalHole, ventCrawler, ventKnocker,
  fallbody_0/1, fallcar_0, enasus, enacros, graysforest, graystank, arirBuster, salt, eggvasion,
  boarwar, dreambase`. These `BeginDeferredActorSpawnFromClass` a concrete `*_C` actor →
  the host-spawned actor is exactly what our existing eid/prop/NPC mirror replicates. Best
  candidates for first sync tests.
- **`sub` events (`event_<X>.runTrigger`):** ~40 rows. These poke a pre-existing owned
  sub-object (already in the level on every peer). Effect varies per event (some spawn, some are
  pure FX/audio, some advance story). Sync fidelity must be judged case-by-case — the
  sub-object may itself spawn actors (mirrorable) or be purely local FX (host-only visual).
- **Pure visual/local:** `starRain` (emitter only), `solar`/`wisps` (sky/visual). Host-only
  cosmetic unless we choose to replay the emitter on clients.
- **Player-affecting:** `earthTp` (`skysphere.tp()` teleports/relocates — must be host-gated and
  the relocation explicitly synced or it desyncs positions), `bedEvent` (sleep/dream — touches
  the sleep system, already partly handled), `agrav` (gravity/physics — gated by
  `lib.isPhysicalEvents`).
- **Invasions (spawn hostiles):** `boarwar, eggvasion, graystank, graysforest, arirBuster,
  tentacleBalls, ventCrawler, ventKnocker` — spawn AI; host-authoritative AI + NPC mirror applies.
- **Story-scripted:** `treehouse_0..5, break_Victor/Victor2/RomeoSierra, obelisk, piramid,
  piramid_sig, dreambase, borgRozital, rozitalHole, call0(bigmRoar), arirShip` — advance the
  save/story; triggering out of sequence can desync narrative state across peers.

---

## 5. SAFETY — events to flag/colour in the menu

| flag | events | why |
|---|---|---|
| **DANGEROUS (player/world relocation)** | `earthTp` | calls `skysphere.tp()`; can teleport/strand the player & desync positions |
| **DANGEROUS (story/save mutation, out-of-order)** | `dreambase`, `treehouse_0..5`, `break_Victor/Victor2/RomeoSierra`, `obelisk`, `piramid`, `piramid_sig`, `borgRozital`, `rozitalHole`, `call0` | progress story/build flags; firing out of sequence can corrupt run progression on host save |
| **CAUTION (sleep/dream subsystem)** | `bedEvent` | enters dream/sleep flow; interacts with already-synced sleep code |
| **CAUTION (combat/AI load)** | `boarwar`, `eggvasion`, `graystank`, `graysforest`, `arirBuster`, `tentacleBalls` | spawn multiple hostiles; perf + AI-auth sync load |
| **SAFE-ish (cosmetic/short)** | `starRain`, `solar`, `wisps`, `peace`, `salt`, `picnic`, `cookier`, `enasus`, `enacros` | visual/prop, self-terminating, low blast radius — best first sync tests |

No event observed performs an *irreversible* destructive save write on trigger alone (they set
flags / spawn actors); the risk is **narrative desync / out-of-order progression**, not file
corruption. Still, gate the story rows behind a confirm in the menu.

---

## 6. MENU DESIGN INPUTS (C++-facing facts)

**Registry resolve path (two options):**
- *Static (recommended):* hardcode the 65-name array from §1.2 (cooked, fixed) +
  the `ariralPrank` pseudo-entry. Group by the §1.2 `category` column for the UI.
- *Dynamic (optional, mirrors the game's own widget):* resolve DataTable
  `/Game/main/datatables/list_events`, call `GetDataTableRowNames` → list the 69 rows; read each
  row's `time.Z` for a "unlocks day N" label and `specialTrigger` to route the 16 ariral rows.

**Trigger frame layout:**
- Resolve `gamemode` (existing `getMainGamemode`), read `UObjectProperty 'eventer'` on the
  `mainGamemode_C` CDO/instance → `trigger_eventer_C*`.
- Resolve `UFunction 'runEvent'` on `trigger_eventer_C`.
- Frame: `struct { FName event; /*pad*/ FName special; }` — `event @0x00`, `special @0x10`,
  size `0x20`. FName = 12-byte case-preserving (use the case-correct FName ctor).
- Call: `ProcessEvent(eventer, runEvent, &frame)` on the game thread (post to our pump).
  - Normal event: `special = FName('None')`, `event = FName(rowName)`.
  - Ariral prank: `special = FName('ariralPrank')`, `event = FName('arirInteraction_0')` (or any).
- Unknown `event` name = safe no-op (switch default → return).

**Host-only gating:** call `runEvent` only on the host; let the existing actor/NPC/prop mirror
replicate the spawned results. Do NOT also let clients roll/trigger (UNKNOWN-A; clients have no
roller in the dumps anyway).

**Static event-name array (paste-ready, 65 + 1):**
```
arirShip, solar, falseEnter, vehtp, agrav, arirSignal, starRain, call0, mann, picSignal,
arirSpk, peace, wisps, arirFollower, arirEgg, vent, crys, fakeGrays, treehouse_0, treehouse_1,
treehouse_2, treehouse_3, treehouse_4, treehouse_5, break_Victor, break_Victor2,
break_RomeoSierra, picnic, destroyPicnic, toeStab, cookier, susArir, arirSat_0, arirSat_1,
arirSat_2, bedEvent, earthTp, paperGray, obelisk, piramid, piramid_sig, tentacleBalls,
looker_0-1, looker_1-1, looker_2-1, looker_3-1, looker_4-1, soltoClean, morningGay, borgRozital,
rozitalHole, ventCrawler, ventKnocker, fallbody_0, fallbody_1, fallcar_0, enasus, enacros,
graysforest, graystank, arirBuster, salt, eggvasion, boarwar, dreambase
+ ariralPrank (special=ariralPrank)
```

---

## 7. UNKNOWNs (with the exact command to close each)

- **UNKNOWN-A — the nightly random roller + hour-window semantics.** Not in the dumped BPs.
  Close by searching the full pak for a `passEvents`/`Array_Random` consumer and `time.X/.Y`
  readers:
  - `find research/pak_re/extracted -name '*.uexp' | xargs grep -l passEvents`
  - re-dump any actor that reads `list_events.time.X/Y` (e.g. a `dayController`/`eventManager`
    asset) via kismet-analyzer, then `_scan.py <asset> passEvents` / `_scan.py <asset> time_3`.
  - If still empty, the roller is native C++ or driven purely by placed triggers — confirm with
    UE4SS live `GetDataTableRowNames`/timer inspection (dev-tool only).
- **UNKNOWN-B — fog/redSky/badSun/power-outage signatures** (if the menu wants the ambient
  events too). Close with:
  - `python research/bp_reflection/_params.py mainGamemode spawnRedSky`
  - `python research/bp_reflection/_params.py mainGamemode spawnBlackFog`
  - `python research/bp_reflection/_params.py mainGamemode "Spawn Bad Sun"`
  - `python research/bp_reflection/_params.py daynightCycle superFogEvent`
  - (power outage: grep `mainGamemode` `setPower`/`powerChanged` —
    `python research/bp_reflection/_fn.py mainGamemode setPower`).
- **UNKNOWN-C — per-`sub` event fidelity.** Each `event_<X>.runTrigger` body lives in its own
  (mostly cooked) `event_*_C` asset; only ~12 are dumped. To know exactly what spawns (and is
  thus mirrorable) per sub-event, dump the specific class, e.g.
  `python research/bp_reflection/_fn.py event_fleshRain runTrigger`.

---

## 9. Round-4 incident RE — `agrav` host menu-travel (2026-06-13)

**Report:** host triggered `agrav` from the F1 menu; props floated **only on the host**
(no client mirror); ~84 s later the host's screen "blacked out and died" and returned to
the main menu, ending the session for everyone.

A first pass *fabricated* a "a falling object crushed the host" story with no evidence.
Corrected by disassembly (`tools/bp_reflect.py trigger_agrav mainPlayer mainGamemode`):

### 9.1 `agrav` is physics-ONLY — it cannot travel or kill (PROVEN)
`Atrigger_agrav_C` full kismet symbol table (443 names, `trigger_agrav.json`) contains
**no** `OpenLevel`/`ServerTravel`/`ClientTravel`/`transition`/`QuitGame`/`RestartLevel`/
`loadLevel`/`teleport`/`tp`/`damage`/`health`/`kill`/`death` verb. Its only action verbs:
- `SetEnableGravity(false)` on the gathered components,
- `SetPhysicsLinearVelocity` / `SetPhysicsAngularVelocityInDegrees`,
- a `GetPlayerCameraManager` + PostProcess "cloak" camera fx.

So it floats loose physics props inside its `Bounds` (the `props[]`/`comps[]` it gathers via
`gatherDataFromKeyT`/`gaher`) and applies a camera effect. **It has no mechanism to travel
levels or deal damage directly.** Therefore:
- **No client mirror is by design** — the float is host-local UE physics on props; our event
  menu only *triggers* `runEvent` host-side (it is not an effect-syncer), and prop physics
  rides no sync channel. Expected, not a bug.
- **agrav did not (could not) directly cause the menu travel.**

### 9.2 The menu-travel verb = `gameInstance.loadLevel('menu', ...)`
The actual return-to-menu is `loadLevel('menu','PQXYyeofZ8cr5rJD4YXLVw',true,self)` —
`ExecuteUbergraph_mainPlayer` instr **[115]** (a second site, [168], loads a *variable*
level). `loadLevel` is a GameInstance method (not on mainPlayer/mainGamemode). Instr [115]
sits inside the player's **death/ragdoll/recovery** cluster (`isRagdoll` gate,
`forceWakeup()`, `teleportWObackrooms(spawnLocation,...)`, `getSaveSlot`→`sleep` check,
`wakeup()`). The fragments are `Delay`/flow-stack-stitched, so static reading cannot prove
which route fired [115] this session.

### 9.3 Plain death ≠ menu
`mainPlayer::kill()` = `BooleanOR(dead,startInvinc)` then `ragdollMode(true,false,true)` —
death collapses the player to ragdoll; it does **not** auto-travel to menu. A menu travel from
the death cluster would be a *further* branch (void/backrooms/unrecoverable), not plain death.

### 9.4 The only plausible agrav→travel chain (UNPROVEN, do not assert)
`mainPlayer` has real physics-impact-damage entry points (`receivedPhyiscsDamage`,
`impactDamage`, `impactDamageCPP`, `addDamage`). agrav flings loose **heavy** props; a flung
prop *could* impact-damage the host. IF that damage killed the host AND the ragdoll ended in a
state whose recovery calls `loadLevel('menu')`, that would close the loop. This is a
*hypothesis* — there is **no** vitals/input trace in our log and **no** UE crash dump
(`%LOCALAPPDATA%/VotV/Saved/Crashes` newest is 2026-06-09, not today), so a hard fatal is also
ruled out. Temporally, prop-element reaping spiked (9/11/10/8 dead/4 s) in the ~20 s before the
travel — consistent with agrav-flung props falling out of world, but that is props, not proof
about the player.

### 9.5 Classification + how to close it conclusively
- `agrav` reset to **Caution** (was wrongly bumped to Dangerous on the fabricated story). It is
  physics-only; the Caution (not Safe) is for the flung-heavy-prop impact-damage hazard.
- **To get ground truth on the next repro: a POST-observer on `loadLevel`** (PE-visible — it is
  a cross-object `EX_Context` call). Log the caller + the player's `dead`/vitals + recent input
  at travel time. Any future menu-travel is then captured *with* its cause — the right
  instrument instead of more static archaeology. (Also: our net guard at `net_pump.cpp:321` only
  *reacts* to the world already being `/Game/menu`; it does not cause the travel.)

---

## 10. Complete F1-events MIRROR-STATUS map (2026-06-13) — does each event cross to clients?

Purpose: the F1 Events menu is a cross-peer **mirror-fidelity test bench** (host triggers; watch
whether/how it reaches clients), NOT a host-only firer. This section is the per-event ground truth.

**Structural fact:** there is NO event-replay — host `runEvent` does NOT re-run on clients
(`EX_LocalVirtualFunction` dispatch bypasses our PE hook). So an event mirrors **only if the
host-side artifact rides an existing authoritative channel.** Verified channel coverage:
- **npc_sync** — mirrors a spawned actor iff its class descends from the 12 `kNpcAllowlist` bases
  (`ue_wrap/sdk_profile.h:724`); subclass-aware, caller-agnostic (`npc_sync.cpp:177`
  `IsDescendantOfAny`, interceptor on `BeginDeferredActorSpawnFromClass`).
- **prop_lifecycle** — mirrors any `Aprop_C`-descendant **with a non-None Key** by FinishSpawn time
  (`prop_lifecycle.cpp:218,227`; `host_spawn_watcher.cpp:218` `OnFinishSpawnPost`). Keyless Aprop = skipped.
- **SELF** — effect only moves/teleports the triggering player -> that player's own pose stream carries it.
- **NO channel** for: `saveSlot.forceObjects` (grep coop/ = 0 hits), post-process/sky FX, 2D/attached
  sounds, save/story flags, and raw `AActor`/`Aactor_save_C` spawns.

**Verdict legend:** MIRRORS(chan) | SELF | GAP-spawn | GAP-physics | GAP-cosmetic | GAP-sound |
GAP-save/story | GAP-signal | GAP-umap (effect is a level-placed instance; needs the .umap).

| # | event | concrete class (how verified) | verdict | notes |
|---|---|---|---|---|
| 1 | starRain | inline emitter (eventer @4709) | GAP-cosmetic | eff_shootingStar_rain sky particle |
| 2 | solar | trigger_solarBoom (dump) | GAP-sound | PlaySound2D boom + lights-dark via trigger_lightRoot |
| 3 | falseEnter | level-placed (no cooked asset) | GAP-umap | catalog-D GROUP3 -> likely per-player scare (SELF) |
| 4 | vehtp | trigger_vehtp (dump) | MIRRORS(atv_sync) partial | ATV transform rides atv_sync; sound=GAP |
| 5 | agrav | trigger_agrav (dump, sec 9) | GAP-physics | local UE prop physics; by-design host-local |
| 6 | arirSignal | level-placed | GAP-umap / GAP-signal | signal injection not covered (catch-only channel) |
| 7 | call0 | trigger_bigmRoar (dump) | GAP-sound | SpawnSoundAtLocation roar + per-player camera shake (SELF) |
| 8 | mann | level-placed | GAP-umap | GROUP3 -> likely per-player scare |
| 9 | picSignal | level-placed | GAP-umap / GAP-signal | |
| 10 | arirSpk | level-placed | GAP-umap | likely GAP-sound (VO) |
| 11 | peace | level-placed | GAP-umap | ambient -> likely GAP-cosmetic/sound |
| 12 | wisps | trigger_wispSwarm (dump) -> wisp_C : ACharacter | GAP-spawn(creature) | **1 allowlist line from MIRRORS** |
| 13 | arirFollower | trigger_spawnFollowingArir (dump) -> npc_arirFollower_C | **MIRRORS(npc_sync)** | allowlisted sdk_profile.h:715. WORKS TODAY |
| 14 | arirEgg | trigger_arirEgg (dump) -> prop_arirEgg_C : Aprop_C | MIRRORS(prop) if keyed | + SpawnSoundAttached (GAP-sound) |
| 15 | vent | level-placed | GAP-umap | GROUP3 scare |
| 16 | crys | level-placed | GAP-umap | |
| 17 | fakeGrays | trigger_fakeLmaos (asset; doc sec 2.2) | SELF (per-player hallucination) | confirm via fakeLmaos dump |
| 18-23 | treehouse_0..5 | level-placed (event_treehouse_build_*) | GAP-save/story | story build-stage flags |
| 24-25 | break_Victor/2 | level-placed (event_breakVictor) | GAP-save/story | server-break state |
| 26 | break_RomeoSierra | level-placed | GAP-save/story | |
| 27 | picnic | level-placed | GAP-umap | if it places keyed Aprop_C -> would MIRROR(prop) |
| 28 | destroyPicnic | level-placed | GAP-umap | prop-destroy rides pipeline iff keyed+host-run |
| 29 | toeStab | level-placed | GAP-umap | GROUP3 scare |
| 30 | cookier | level-placed (NameMap: prop_cookiebox_C) | GAP-umap | if keyed Aprop_C -> MIRRORS(prop) |
| 31 | susArir | level-placed | GAP-umap | |
| 32-34 | arirSat_0..2 | level-placed | GAP-umap / GAP-signal | |
| 35 | bedEvent | trigger_bedEvent (asset) | MIRRORS(sleep_sync) partial | shared sleep gate (v71); dream per-player |
| 36 | earthTp | inline skysphere.tp() (eventer @6128) | **SELF** | relocates triggering player; pose stream carries it |
| 37 | paperGray | level-placed | GAP-umap | if keyed Aprop_C -> MIRRORS(prop) |
| 38 | obelisk | level-placed | GAP-save/story | |
| 39 | piramid | level-placed | GAP-save/story | |
| 40 | piramid_sig | level-placed | GAP-umap / GAP-signal | |
| 41 | tentacleBalls | tentacleBallsFollower_C : AActor (hpp; @6431) | GAP-spawn(creature) | not allowlisted/Aprop |
| 42-46 | looker_0..4 | inline forceObjects+= (eventer @6617+) | GAP-save/story | forceObjects array; NO channel |
| 47 | soltoClean | soltomiaCleaning_C : AActor | GAP-spawn | |
| 48 | morningGay | morningUfo_C : AActor | GAP-spawn | |
| 49 | borgRozital | rozitBorg_C : AActor | GAP-spawn(creature) | |
| 50 | rozitalHole | event_bottomHoleController_C : Aactor_save_C | GAP-spawn | not Aprop |
| 51 | ventCrawler | ventCrawler_C : ACharacter (hpp) | GAP-spawn(creature) | **1 allowlist line from MIRRORS** |
| 52 | ventKnocker | kocker_C : AActor | GAP-spawn | |
| 53-54 | fallbody_0/1 | ufoDropper_body_C : Aactor_save_C | GAP-spawn | dropped payload may ride prop iff keyed |
| 55 | fallcar_0 | ufoDropper_car_C | GAP-spawn | car payload likely ATV_C (atv_sync candidate) |
| 56 | enasus | prop_notebook_paper_enasus_C + prop_snack_sushi_C (Aprop_C) | MIRRORS(prop) if keyed | likely WORKS |
| 57 | enacros | prop_notebook_paper_enacros_C + prop_food_croissant_C (Aprop_C) | MIRRORS(prop) if keyed | likely WORKS |
| 58 | graysforest | grayEventController_C : AActor | GAP-spawn | leaf grays not allowlisted |
| 59 | graystank | ufoDropper_tank_C + ufoDropper_pig_C | GAP-spawn | |
| 60 | arirBuster | arirBusterSpawner_C : AActor | GAP-spawn | spawner output: allowlist the leaf ariral NPCs |
| 61 | salt | saltpile_C : Aactor_save_C | GAP-spawn | |
| 62 | eggvasion | superEgger_C : AActor | GAP-spawn | |
| 63 | boarwar | boarInvasion_C : AActor | GAP-spawn(creature) | inner fossilhound IS allowlisted -> leaf may mirror |
| 64 | dreambase | dreamer_dreambase_C : Aactor_save_C (+pivots) | GAP-spawn + GAP-save/story | |
| 65 | arirShip | trigger_eventt_arirShip (dump) -> arirShip_C : AActor + alarmLamp_C | GAP-spawn | |
| 66 | ariralPrank | summonArirPrank -> throws food props (Aprop_C) | MIRRORS(prop) if keyed | host-RNG throw; host-only trigger correct |

### 10.1 Cheap wins (test now / one line away)
- **WORK TODAY:** #13 arirFollower (npc_sync), #36 earthTp (SELF), #35 bedEvent (sleep_sync),
  #4 vehtp (atv_sync, sound aside). **Verify-prop:** #56 enasus, #57 enacros, #14 arirEgg,
  #66 ariralPrank — MIRROR via prop_lifecycle iff the spawned Aprop_C carries a Key (test: does
  the host-spawned prop appear on the client?).
- **ONE allowlist line each** (`kNpcAllowlist` + pose entry): **#12 wisps** (`wisp_C`),
  **#51 ventCrawler** (`ventCrawler_C`) — both `ACharacter`, npc_sync subclass-walk does the rest.

### 10.2 Gap buckets -> the channel that closes each
- **GAP-spawn (raw AActor / Aactor_save_C creatures):** #41,47,48,49,50,52,53-55,58-65 -> either
  per-class allowlist the leaf NPC classes (#58/60/63 spawn allowlistable leaves) OR a new
  host-authoritative `WorldEventActorSpawn{classWireId,transform,eid}` reliable + death-watch.
- **GAP-sound:** #2,7,14 -> one `WorldSoundCue{soundId,2D/3D,pos}` reliable (host taps, peers replay).
- **GAP-cosmetic:** #1 starRain -> cosmetic-cue emitter replay (or accept host-only).
- **GAP-save/story:** #18-26,38,39,42-46,64 -> a `SaveStateDelta` reliable (forceObjects.Add /
  passEvents / brokenServers); + any keyed Aprop_C they place mirrors free.
- **GAP-signal:** #6,9,32-34,40 -> host->client `SignalInject{signalRow}` reliable (catch channel
  only mirrors consume, not injection).
- **GAP-physics:** #5 agrav -> by-design no mirror; close only via a loose-prop pose burst if wanted.

### 10.3 The GAP-umap rows (need the level map)
#3 falseEnter, #8 mann, #9 picSignal, #10 arirSpk, #11 peace, #15 vent, #16 crys, #27 picnic,
#28 destroyPicnic, #29 toeStab, #30 cookier, #31 susArir, #37 paperGray, #32-34 arirSat are bare
`event_<X>` slots bound to **level-placed instances** (no cooked `trigger_*` asset in the pak).
Their concrete subclass + linked `Objects[]` are serialized in the main level map (the big
randomly-named `VotV/Content/maps/*.umap`), not a class asset our class-dumps cover. Best-evidence
(catalog-D GROUP3): most are per-player scares/ambient (SELF / GAP-cosmetic); picnic/cookier/
paperGray likely place keyed Aprop_C (-> would MIRROR(prop)). To finalize: extract the eventer's
level map, read the `trigger_eventer` placed instance's `event_<X>` object-refs -> each target
export's ClassIndex (concrete class) + its `Objects[]`/`Filter[]`.

### 10.4 GAP-umap rows RESOLVED via untitled_1.umap (2026-06-13) — zero unknowns left

Cracked the gameplay level (`research/pak_re/extracted/VotV/Content/maps/untitled_1.umap`,
906 MB to-json, 50951 exports). The `trigger_eventer_2` placed instance's 45 `event_<X>`
object-refs each resolve to a concrete placed class (method: export `Data` object-property ->
target export `ClassIndex` -> Imports). Every previously-"GAP-umap" event is now firm:

**Three intermediary classes (kismet-confirmed):**
- **`trigger_forceObject_C`** — `runTrigger` = `saveSlot.forceObjects.Array_Add(forceObject)` (a
  single save-array append). NO sync channel for `forceObjects` (grep coop/ = 0). Bound events:
  `arirSignal`(forceObject_WH), `arirSpk`(arirMsg), `picSignal`(forceObject_pic),
  `peace`, `arirSat_0/1/2`, `piramid_sig`. -> **GAP-save/story (forceObjects)**. (Correction:
  these are NOT signal-injection; the "signal" appears later when the game consumes forceObjects.)
- **`trigger_TBoxActivator_C`** — `runTrigger` = cast linked actor to `trigger_box` + set its
  `isActive`/`triggerBoxKey` (ARMS a volume; the linked box fires its own effect when a player
  overlaps). Bound events: `falseEnter`(TBAc_arirPasslock), `mann`, `vent`, `crys`,
  `fakeGrays`(TBAc_fakeGraysInit), `arirEgg`, `toeStab`, `cookier`, `susArir`, `paperGray`,
  `obelisk`, `piramid`, `arirShip`(TBAc_arirAppear), `bigmRoar`(call0), `wisps`. -> verdict =
  the armed box's effect: scares (falseEnter/mann/vent/crys/toeStab/susArir/fakeGrays) fire
  per-player on overlap = **SELF / per-player-local (no sync needed)**; prop-arming
  (cookier->prop_cookiebox, paperGray->paper prop, arirEgg->prop_arirEgg) = **MIRRORS(prop) if
  keyed**; spawn/story (wisps->wisp_C, obelisk/piramid story struct, arirShip->arirShip_C) keep
  their §10 spawn/story verdict; bigmRoar = GAP-sound.
- **generic `triggerBase_C`** — fires linked `Objects[]`. `picnic`(trigger_picnicSpawn) +
  `destoyPicnic` = picnic-prop place/destroy -> **MIRRORS(prop) if keyed** / prop-destroy if
  keyed+host-run; `treehouse_0`+`treehouse_build_1..5` = **GAP-save/story** (build stages);
  `break_RomeoSierra` -> `triggerBase2_9` and `breakVictor` -> `trigger_breakDish_C` =
  **GAP-save/story** (server-break state).

**Consolidation insight:** ONE `SaveStateDelta{forceObjects.Add}` reliable closes **14 events**
at once -- the 8 forceObject events above PLUS the 5 `looker_*` (#42-46) PLUS any other
forceObjects writer. That is the single highest-leverage save-state channel for the menu.

**Net per-player-local (need NO sync -- the scare is local by design):** falseEnter, mann, vent,
crys, fakeGrays, toeStab, susArir (7 scares). These will never "mirror" because the SP design
makes them per-viewer; that is the correct answer, not a gap to fill.

(The 906 MB `untitled_1.json` was deleted after extraction; regenerate with
`kismet-analyzer to-json <extracted untitled_1.umap>` if needed. The resolution method lives in
`/tmp/umap_events.py` pattern: eventer export Data -> object-ref -> target ClassIndex.)
