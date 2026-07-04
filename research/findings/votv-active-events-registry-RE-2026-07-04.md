# VOTV native ACTIVE-EVENT registry + the no-save-during-event gate — RE (2026-07-04)

**Durable RE** (bytecode-verified from the 0.9.0n pak extraction; tools: kismet-analyzer
to-json + `research/bp_reflection/_fn.py`). This is the ground truth the join-during-event
design (`docs/COOP_EVENT_JOIN.md`) stands on.

## 1. The game's own "event in flight" registry (mainGamemode)

Two properties on `mainGamemode_C` (declared there; NEVER written by gamemode's own
bytecode):

- `activeEvents` : **int counter** — how many events are currently in their active phase.
- `activeEvents_senders` : **TArray<UObject*>** — the LIVE event actor of each in-flight
  event (the `__WorldContext` the actor passed when registering).

### The single writer: `lib_C::setEvent(WorldContext, isEventActive, deactivateAmbientTrack)`

Decoded (`_fn.py lib setEvent`, 147 stmts):

- `isEventActive == true`:
  - `gamemode.activeEvents += 1`
  - `Array_Add(gamemode.activeEvents_senders, __WorldContext)`  ← the sender REGISTERS ITSELF
  - if `deactivateAmbientTrack`: `gamemode.disableEventTrack += 1` (music suppression counter)
  - plus an on-screen/log Format() of the in-game time (the "event started" console line)
- `isEventActive == false`:
  - `gamemode.activeEvents -= 1`
  - `Array_RemoveItem(gamemode.activeEvents_senders, __WorldContext)`
  - if `deactivateAmbientTrack`: `disableEventTrack -= 1`
  - clamp: `if (activeEvents < 0) activeEvents = 0`

So the registry is a classic refcount + membership list, maintained by the event actors
themselves (RAII-style: register at active-phase start, deregister at end).

### The reader: `lib_C::getEvent(WorldContext) -> bool isEventActive`

Decoded (10 stmts):

```
isEventActive = (gamemode.activeEvents > 0)
                OR NOT IsPointInBox(cameraManager.location, <base-area box>, <extent>)
```

i.e. "an event is running, OR the player is outside the base area box" (away-from-base
counts as unsafe-to-save; the box literals are in the bytecode @172).

## 2. The no-save-during-event gate (user-reported native behavior — mechanism CONFIRMED)

Two independent enforcement points, both bytecode-verified:

1. **UI**: `ui_menu.enterPause` (decoded) calls `lib_C::getEvent`; if true:
   `button_Save.SetIsEnabled(false)` + `button_overwriteBackupSave.SetIsEnabled(false)`
   **and skips `SetGamePaused(true)`** — the game does NOT pause while an event is active.
   (Else-branch: pause + enable both buttons.)
2. **Save funnel**: `saveSlot_C::save` (decoded) — first gate is
   `IF (gamemode.disableSave) -> abort` (EX_PopExecutionFlow before saveObjects/saveToSlot).
   `disableSave` is a SEPARATE bool (writers among extracted assets: only
   `sl_poopDim.umap`'s level script — the dream dimension), i.e. the event block is
   enforced via the UI gate (1), not via disableSave. The only other save gate in
   `saveSlot.save` is `IsEmpty(gameInstance.slotName)`. mainGamemode also has a
   "Cannot save in mid air" gate string (its own pre-save check, separate seam).

**Coop implications:**
- Our join live-capture (`ue_wrap/save_capture.cpp`) calls `saveObjects`/`saveTriggers` +
  `GameplayStatics::SaveGameToSlot` DIRECTLY — it enters below both gates, so a mid-event
  join capture mechanically SUCCEEDS. Event actors are not `int_save_C`, so the blob is
  the clean BASE world without event transients — the exact base layer the join design
  wants. The gates exist because a RELOAD mid-event would lose the event; that is a
  design fact about the save format, not a blocker for our capture.
- Our coop no-pause invariant (pause_guard) matches native semantics MORE closely than we
  knew: SP itself refuses to pause during events.

## 3. Census: every class with an active event phase (the setEvent callers)

`grep -rl setEvent` over the full pak extraction (3046 uassets + 261 umaps) — ~95 assets.
Notables (full list reproducible by the grep):

- **Story/scheduled events**: `piramid2` (the pyramid!), `obelisk`, `prop_obelisk`,
  `rozitBorg`, `roz_anim`, `arirShipAppear`, `arirShip_tower`, `badSun`, `superFog`,
  `blackFog`, `skyFallingEvent`, `event_fleshRain`, `event_consoleWrite`,
  `event_fossilBoarWar`, `morningUfo`, `skyUfo`, `ufoAbducter`, `ufoDropper`,
  `ufo_{ballfo,boofo,joel,pillfo,trifo}`, `tardis`, `outsideChurch`, `stairs`, `eg`.
- **Creature controllers**: `killerwisp`, `ventCrawler`, `centipedeHead`, `eyer`,
  `fossilhound`, `grayboar`, `grayEventController`, `zombieHordeController`,
  `kavotiaPatrolController`, `wendussy`, `murderKerfur`, `kerfurOmega`, `tomtenisse`,
  `skerfuroWalk`, `npc_{arirFollower,arirGunStealer,ariral_shooter,funguy,krampus,orborb,
  angryErieFlesh}`, `tentacleBallsFollower`, `soltomiaCleaning`.
- **Ariral pranks**: `fuelingArir`, `arirTrasher`, `alienJump`, `hillRollerSpawner`,
  `cookier`, `antibreather`.
- **Ambient/misc**: `noiser`, `noiser1`, `newsky`, `blooder`, `puddle`, `goat`, `rufus`,
  `birch`, `erieChop`, `skullzone`, `easterZone`, `superHolyShitAngryBunny`,
  `firetankDeploy`, `fridgeGlow`, `prop_{dingus,fridgeDoor,llama,vending}`, `ATV`,
  `trigger_{agrav,alarm,lockerLooker,tpChamberSpawn}`, `lightningHeightAnalyzer`,
  `RCM_cameraManager`, `midasufotest`, `analogd/firetank/ufoshieldshader` (test).
- **Sublevel scripts**: `sl_{alexpdim,caves,goop,place,backroomsObsolete}`, `sl_poopDim`
  (also the one disableSave writer), `untitled_219`.

## 4. Seam quality for coop

- **Host-side observation**: `activeEvents` + `activeEvents_senders` are plain gamemode
  properties — a 1 Hz poll + diff (the proven passEvents-poll shape from
  `event_fire_sync.cpp`) yields event START (sender appears; ClassOf(sender) names the
  event) and END (sender removed) with zero hooks. `setEvent` itself is a lib_C CDO call
  from BP internals (EX-dispatch visibility NOT needed — the poll reads the result).
- **Sender identity**: `ClassOf(activeEvents_senders[i])` is the event's implementation
  class (e.g. `piramid2_C`), NOT the list_events row name — a class->row mapping table is
  part of the design (docs/COOP_EVENT_JOIN.md).
- Tooling note: `research/bp_reflection/_fixjson.py` repairs kismet-analyzer JSON that
  UAssetAPI emits with unescaped quotes in localized strings (the saveSlot.json breakage
  class); `ui_menu_fixed.json` produced this run.
