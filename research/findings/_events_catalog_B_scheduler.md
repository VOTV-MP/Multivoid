# VOTV Event System — Catalog B: the Central Scheduler (RE, bytecode-grounded)

> **Date:** 2026-06-11 · **Engine:** UE4.27 STOCK non-nativized (all BP = Kismet
> bytecode). **Method:** `tools/bp_reflect.py` disassembly (kismet-analyzer JSON in
> `research/bp_reflection/`) + CXX header dump
> (`Game_0.9.0n/.../CXXHeaderDump/`). Every claim below cites the disassembled
> bytecode expression index `[N]` of the named function, or the DataTable rows.
>
> **TL;DR VERDICT:** VOTV has **two independent event subsystems and NO single
> observable central roll**. (A) **Scripted story events** are a fixed
> time-scheduled DataTable (`list_events`, 69 rows keyed by day/hour/minute),
> fired **deterministically** by `saveSlot::settime` → `eventer->runEvent(name,
> special)` — there is no RNG roll, the "decision" is just *the clock crossing a
> fixed timestamp*. (B) **Ambient ticker spawners** each roll their **own** RNG
> locally per-peer (every `Aticker_base_C` actor + the per-hour rolls in
> `daynightCycle`). The fire dispatch in BOTH cases is `EX_LocalVirtualFunction`
> (`runEvent`/`runTrigger`/`spawn`) → `ProcessInternal`-direct, which **bypasses
> our ProcessEvent detour** (the playerGrabbed / firefly trap). So **for (A) the
> leverage point is the CLOCK, not a roll** (sync game time + the fired-set —
> already largely in place via time_sync); for (B) it is **per-event output
> detectors** (the firefly precedent), there is no central seam to intercept.

---

## 1. THE EVENT MODEL — what a VOTV "event" is

There is **no single "event" abstraction**. The word "event" spans three
distinct mechanisms:

### 1a. The scripted story schedule — `list_events` DataTable (THE event table)

`VotV/Content/main/datatables/list_events.uasset` — a `UDataTable` of
**`Fstruct_event`**, **69 rows**, each row **keyed by name**. There is **no
event enum**; the "event id" is the **row FName**.

`struct_event.hpp` (the entire event-data struct — deliberately tiny):
```cpp
struct Fstruct_event {
    FIntVector time;          // 0x0000  (X=hour, Y=minute, Z=DAY)   <- the SCHEDULE
    FName      specialTrigger; // 0x000C  (e.g. n'ariralPrank', else None)
}; // Size: 0x14
```
So an "event" = **{name, scheduled (day,hour,minute), optional specialTrigger}**.
No weight, no cooldown, no spawn-class, no condition flags in the struct — the
schedule **IS** the condition, and the row name maps to a pre-placed trigger
actor (§3). The `time` is **(hour, minute, day)** — verified by the values:
`arirShip=(0,1,12)` (day 12, 00:01), `arirSat_0/1/2=(11/12/13:00, day 25)`,
`solar=(0,10,3)`, `starRain=(0,17,1)`. Rows are sorted ascending by **day (Z)**.

All 69 rows (name → time(h,m,**day**) → special):
```
starRain (0,17,1)   solar (0,10,3)    ventCrawler (23,27,4)  peace (23,14,5)
picnic (0,44,7)     break_RomeoSierra (1,44,7)              arirInteraction_0 (0,6,8) ->ariralPrank
arirSignal (18,0,8) destroyPicnic (0,0,9)  arirSpk (0,2,10) ventKnocker (0,38,11)
picSignal (23,10,11) arirShip (0,1,12)     arirInteraction_1 (17,25,13)->ariralPrank  wisps (19,17,13)
enasus (8,49,14)    arirBuster (17,0,14)   arirInteraction_2 (22,0,15)->ariralPrank   treehouse_0 (0,0,16)
agrav (1,0,16)      break_Victor (1,11,16) arirInteraction_3 (23,0,16)->ariralPrank   treehouse_1 (0,0,17)
arirInteraction_4 (19,13,17)->ariralPrank  treehouse_2 (0,0,18)  arirInteraction_5 (2,0,18)->ariralPrank
treehouse_3 (0,0,19) enacros (8,49,19)     arirInteraction_6 (2,0,19)->ariralPrank    break_Victor2 (1,2,19)
arirInteraction_7 (18,0,20)->ariralPrank   treehouse_4 (0,0,20)  arirInteraction_8 (20,0,21)->ariralPrank
treehouse_5 (0,0,21) arirInteraction_9 (21,27,22)->ariralPrank  toeStab (2,3,23)
arirInteraction_10 (3,30,24)->ariralPrank  salt (18,0,24)  cookier (5,30,24)  obelisk (15,40,24)
arirSat_0 (11,0,25) arirSat_1 (12,0,25)    arirSat_2 (13,0,25)  arirInteraction_14 (1,11,26)->ariralPrank
earthTp (11,27,26)  looker_0-1 (20,0,27)   looker_1-1 (22,0,27) looker_2-1 (0,0,28)
arirInteraction_15 (0,11,28)->ariralPrank  looker_3-1 (2,0,28)  looker_4-1 (4,0,28)
piramid_sig (18,0,29) piramid (18,0,30)    dreambase (18,0,30)  arirInteraction_13 (2,33,31)->ariralPrank
tentacleBalls (18,0,32) arirInteraction_11 (1,44,33)->ariralPrank  soltoClean (19,0,34)
arirInteraction_12 (3,33,35)->ariralPrank  morningGay (10,50,36) borgRozital (11,0,37) rozitalHole (0,0,38)
fallbody_0 (23,11,39) fallbody_1 (4,18,40) fallcar_0 (22,28,41) graysforest (17,1,42)
graystank (18,1,43) boarwar (3,0,46)       call0 (0,1,47)
```
This is the **one-shot main-story timeline**: each event fires once at its
scheduled day/time. Persistent state lives in the saveSlot (§2): `allEvents`
(all 69 names), `passEvents` (already-fired / skipped).

### 1b. The ambient ticker spawners — `Aticker_base_C` family (33 classes)

`VotV/Content/objects/tickers/ticker_*` — 33 actors, each `: Aticker_base_C :
AActor`. Examples: `ticker_fireflySpawner`, `ticker_mannequinSpawner`,
`ticker_deerSpawner`, `ticker_wispSpawner`, `ticker_insomniacSpawner`,
`ticker_roachSummoner`, `ticker_beehiveSpawner`, … Each is just
`ReceiveTick` + an ubergraph; `Aticker_base_C` holds only a `GameMode*` and
subscribes to gamemode interface broadcasts (`intComs_*`). These are the
**continuous ambient-spawn rollers** — NOT in `list_events`, NOT story-gated.

### 1c. The per-event ACTIVE-tracking refcount — `lib_C::getEvent/setEvent`

Not a roller — a **refcount of currently-running events** used to (i) suppress
ambient music during events and (ii) tell other systems "an event is active."
`mainGamemode` fields: `int32 activeEvents` (0x0E68), `TArray<UObject*>
activeEvents_senders` (0x0E70), `int32 disableEventTrack` (0x0E98).

`lib_C::setEvent(isEventActive, deactivateAmbientTrack, __WorldContext)`:
- `[52-55] activeEvents += 1`; `[57] activeEvents_senders.Add(__WorldContext)`.
- `[58-63]` if `deactivateAmbientTrack`: `disableEventTrack += 1`.
- `[50] addToMetaPLog(...)` — logs the event start (day/hour + display name +
  timestamp) to the "metaP" log.

`lib_C::getEvent(__WorldContext) -> isEventActive`:
`[3] activeEvents > 0` **OR** `[5] player NOT in base box` → returns true.

`mainGamemode` ubergraph `[3027-3032]`:
`getEvent(...) AND disableEventTrack<=0 AND useMetaPAudio → threatAmb->Play()`
(this is where the active-event flag gates the ambient/threat music track).

So each `event_*` actor calls `setEvent(true)` on start and `setEvent(false)`
on end; `activeEvents` is the live count. **This is event STATE, not the roll.**

---

## 2. THE ROLL / SCHEDULE — who decides an event fires, and WHICH

### Scripted events: `saveSlot::settime` — a DETERMINISTIC clock-cross check (NO RNG)

The master clock setter is **`saveSlot::settime(FIntVector NewTime, out
Output_Get, out new_min, out new_hour, out new_day)`** (`saveSlot.hpp:141`). It
is called whenever game time advances (the daynightCycle drives it). Inside it,
for each event row it does (saveSlot.json, function `settime`, bytecode region
~`[lines 4131-4389]`):

```
conv_RV   = conv( row.time )      // [4131-4187] event scheduled (d,h,m) -> scalar
conv_RV_1 = conv( savedtime )     // [4188-4232] CURRENT save time     -> scalar
Greater_IntInt( conv_RV_1, conv_RV )   // [4234-4280]  currentTime > eventTime ?
POPFLOWIFNOT(...)                       // [4282-4295]  skip if not yet due
gamemode->eventer->runEvent( n, row.specialTrigger )   // [4298-4389]
```

- **The "roll" is the clock.** The decision "event E fires" is purely
  `currentTime > E.scheduledTime` — a deterministic timestamp comparison. There
  is **no `RandomBool`, no `weightedRandom`, no `Array_Random`** anywhere in the
  scheduled-event path (grep-verified across `settime`'s region and the
  trigger_eventer ubergraph: 0 RNG nodes). Cadence: re-checked every time the
  clock ticks a minute (settime is the per-time-step setter).
- **Persistent dedupe:** at boot (`mainGamemode` ubergraph `[1380-1406]`)
  `saveSlot.allEvents = GetDataTableRowNames(list_events)`, and for a save
  started on `startDay`, every row whose **day (Z) `< startDay`** is appended to
  `saveSlot.passEvents` `[1396-1399]` (past events marked already-passed). So
  `settime` only fires rows not in `passEvents` (the fired-set lives in the
  save).
- **Host vs each-peer:** `saveSlot::settime` runs on **whoever owns the
  authoritative clock**. If both peers run their own daynightCycle/settime,
  **both would independently fire the same scheduled events** (and re-roll the
  embedded ambient RNG, §1b/§4) — the divergence risk. But because the trigger
  is a **deterministic function of game time + the persistent passEvents set**,
  syncing those two pieces of state makes all peers fire identically. **This is
  the key asymmetry vs the ambient rollers: scripted events are reproducible
  from synced state.**

There is also a **manual/dev launcher**: `Uui_eventRun_C` (a debug widget
listing all `list_events` rows) — on button click (`ExecuteUbergraph_ui_eventRun
[6]`): `gamemode->eventer->runEvent(name, row.specialTrigger)`. Not used in
normal play; confirms the same single entry point.

### Ambient tickers: each actor rolls its OWN RNG, locally, per-peer

There is **no central ticker counter** the gamemode advances. "ticker" =
the family of independent `Aticker_base_C` actors, each self-arming via RNG:

- **`ticker_mannequinSpawner`** ubergraph: `[74] SetActorTickInterval(
  SelectFloat(RandomFloatInRange(5400,6300), RandomFloatInRange(6300,10800)))`
  — re-arms itself with a **random 90 min–3 hr** interval, gated by
  `[68] daynightCycle->timeZ >= 14` (night); then `[77] RandomBoolWithWeight(
  gamemode->mannDest)` + `[80] Array_Random(allManns)` to pick spawn point →
  `[83] BeginDeferredActorSpawnFromClass(prop_C) [87] FinishSpawningActor
  [88] K2_DestroyActor`.
- **`ticker_fireflySpawner`** ubergraph (the existing-precedent ambient):
  `[5] RandomBoolWithWeight(...)`, `[9-10] RandomFloatInRange` for angle+radius,
  `[24] SpawnEmitterAtLocation(eff_fireflies, ...)` — pure per-tick local roll.

Every ambient ticker follows this shape (independent `RandomBool*` + random
interval + random position). **Two peers running the same ticker BP roll
different intervals/positions → guaranteed divergence; there is no shared roll
to intercept.** (This is exactly why fireflies (v51) had to be a per-event
output detector.)

---

## 3. THE FIRE — how a chosen event executes, and observability

### Scripted: `trigger_eventer::runEvent(FName Event, FName Special)` → `event_X->runTrigger`

`Atrigger_eventer_C` (`trigger_eventer.hpp`) is a **passive dispatcher**: it
holds ~40 `AtriggerBase_C* event_*` pointers (one per pre-placed story trigger:
`event_arirShip`, `event_solar`, `event_mann`, `event_obelisk`, `event_arirSat_0/1/2`,
`event_treehouse_build_1..5`, …) and exposes `runEvent(Event, Special)` +
`runSpecialEvent(eventName)`. Its own ubergraph has **0 timers and 0 RNG** —
verified. `runEvent`'s body (`ExecuteUbergraph_trigger_eventer` from offset 528)
is a giant **switch on the event FName** that fires the matching pre-placed
trigger:

```
[155] event_arirShip      -> runTrigger(self, 0)
[157] event_solar         -> runTrigger(self, 0)
[173] event_mann          -> runTrigger(self, 0)
[224] event_arirSat_0     -> runTrigger(self, 0)
... (~40 cases)
[149-153] if Special == n'ariralPrank: addHint(...) + summonArirPrank()
```
`runSpecialEvent(name)` is a second, larger FName switch (351 exprs) covering
physical/prank sub-events (`falseEnter`, `fakeGrays`, `mann`, `crys`, `gascans`,
`trashPiles`, `rockThrow`, `hillRoller`, …) → `event_X->runTrigger()` /
`ariralThrowers.throwType(...)`.

`AtriggerBase_C::runTrigger(AActor* Owner, int32 Index)` (`triggerBase.hpp:111`)
is the per-event "go" verb (each `event_X` BP overrides the trigger behavior).

**OBSERVABILITY — the decisive negative result.** The fire calls are
**`EX_Context → EX_LocalVirtualFunction`**, e.g. raw `[155]`:
`ObjectExpression = event_arirShip (InstanceVariable); ContextExpression =
EX_LocalVirtualFunction{ VirtualFunctionName="runTrigger" }`. And the *entry*
itself: `ui_eventRun [6]` and `saveSlot settime [4350-4352]` both invoke
`eventer->runEvent` via **`EX_LocalVirtualFunction`** too. `summonArirPrank`
`[153]` is `EX_LocalVirtualFunction`. **`EX_LocalVirtualFunction` resolves the
UFunction by name on the target's own class and calls it through
`CallFunction`/`ProcessInternal` directly — it does NOT route through
`UObject::ProcessEvent`**, so **our ProcessEvent detour never sees `runEvent` or
`runTrigger`** (the exact `playerGrabbed` / `firefly SpawnEmitter` trap recorded
in MEMORY). **There is no observable seam where the host "learns event E was
chosen" via a UFunction hook.** (The event-data is observable at the *state*
level — `saveSlot.passEvents` gains the name, `activeEvents`/`activeEvents_senders`
change — but not at a call seam.)

### Ambient: `BeginDeferredActorSpawnFromClass` / `SpawnEmitterAtLocation` / `spawn()`

Ambient ticker fires spawn actors/emitters. `BeginDeferredActorSpawnFromClass`
+ `FinishSpawningActor` ARE ProcessEvent-dispatched (the npc_sync precedent), so
the *spawned actor* is observable; but the **roll that chose it is local RNG**
(§2), and `SpawnEmitterAtLocation` is `EX_CallMath` (firefly trap — bypasses
ProcessEvent). Net: ambient must be mirrored at the **output** (detect the
spawned actor/PSC and broadcast), never at a roll.

---

## 4. THE TICKER — what "ticker" is, and a SECOND smaller leverage point

"ticker" is **NOT** a global counter; it is the `Aticker_base_C` actor family
(§1b) plus a set of **per-hour ambient rolls hard-coded in `daynightCycle`'s
ubergraph**, all fired on the `settime` `new_hour` pulse and all **local RNG**:

`daynightCycle` ubergraph, hour-pulse region `[283-312]`:
```
[286] RandomBoolWithWeight(0.007)  -> jellyfishPath->spawn()           [288]
[291] RandomBoolWithWeight(0.0001) -> spawn event_fleshRain_C          [294-296]
[308] RandomBoolWithWeight(0.005)  -> skysphere->setEye(true)          [310]
(+ random ambient music / thunder / rain re-rolls, [318-344])
```
plus separate self-arming timers in the same BP (`timerLightning`, `timerRain`,
`permaRain_timer`, `timer_changeSunRotation`) — each `RandomFloatInRange` +
`Delay`, all local.

The clock itself (`daynightCycle.timeZ`, advanced into `saveSlot::settime`) IS a
single authoritative value. **Syncing the clock (host-authoritative time, which
`time_sync` v36 already does) automatically aligns: (a) every scheduled event in
`list_events` (they key off the same time), and (b) the `daynightCycle`
per-hour rolls' *gating* — though the embedded `RandomBool*` outcomes inside
each hour pulse still diverge unless detected at output.** So the ticker gives a
**partial** leverage point: time-sync aligns *when* the gates open and fires all
scripted events deterministically, but the *ambient RNG inside* the gates is
still per-peer.

---

## 5. COOP-SYNC LEVERAGE VERDICT

**Split the two subsystems — they need opposite architectures.**

### (A) Scripted story events (`list_events`) → **SYNC THE CLOCK + FIRED-SET, then HOST-DRIVE `runEvent`** (NOT a roll-broadcast, and NOT per-event detectors)

The prompt's option (a) "host broadcasts {eventId, seed}" is **almost right but
mis-framed**: there is **no roll to broadcast** — the decision is a deterministic
function of game time. The clean architecture is:
1. **Host owns the clock** (already true via host-authoritative `time_sync`
   v36). Only the host's `saveSlot::settime` is authoritative for event firing;
   the client's local `settime` must be **prevented from firing events**
   (suppress the `eventer->runEvent` path on the client — runtime-gated, like
   the `changeWindOrigin`/drone-tick suppressions already shipped), so a far
   client never double-fires from its own clock.
2. **Host broadcasts the chosen event** when its `settime` fires:
   `EventFire{ rowName : FName, special : FName }` (tiny — name + special). The
   client calls **the same** `eventer->runEvent(rowName, special)` locally via a
   reflected UFunction call (CDO/Local-ctx invoke — the weather_lightning /
   firefly-spawn precedent), reproducing the exact event. Because the fire is a
   pre-placed `event_X->runTrigger` keyed by the row name, it is **locally
   reproducible** from just the name.
3. **Sync the dedupe set:** mirror `saveSlot.passEvents` (and `allEvents`) so a
   joining/late client doesn't replay past events. This rides the existing
   save-transfer join (v56) for initial state + the per-event broadcast for live
   ones.

Why not per-event detectors for (A): there are ~40+ heterogeneous story events
(ships, satellites, treehouse builds, obelisk, prank interactions) — writing an
output detector per event is the firefly approach × 40, enormous and fragile,
**and unnecessary** because (unlike ambient) the scripted fire is reproducible
from a single FName + the synced clock. Drive it from the one authoritative
`runEvent` instead.

Why not "intercept the roll on the host" literally: `runEvent`/`runTrigger` are
`EX_LocalVirtualFunction` → **not ProcessEvent-observable** (§3), so we cannot
*passively hook* the host's fire. Instead we **actively gate**: the host's
`settime` event path is the trigger — we detect the fire by **diffing
`saveSlot.passEvents` on the host each time-step** (a name appears → that event
just fired → broadcast it), which is cheap (a small FName array compare on the
minute pulse, not per-frame) and avoids needing a ProcessEvent seam. This is the
"host rolls → broadcast RESULT → peers mirror" pattern at the **state-diff**
level, consistent with the user's stated principle and the firefly precedent's
*detection* half.

### (B) Ambient tickers (`Aticker_base_C` + daynightCycle per-hour rolls) → **PER-EVENT OUTPUT DETECTORS** (the firefly model), there is NO central seam

Each ambient spawner rolls independent local RNG (interval, weight, position) —
**option (a) is impossible** for these (no shared roll, `SpawnEmitter` via
`EX_CallMath` bypasses ProcessEvent). The only correct architecture is option
(b): **host runs the spawner, detects the spawned actor/PSC, broadcasts it, peers
mirror** — exactly the shipped `firefly_sync` (peer-symmetric PSC-diff + spawn
relay) and the ATV/drone/NPC pose precedents. Each ambient species that matters
for coop gets its own detector+mirror (fireflies done; mannequin/deer/wisp/etc.
as scoped). **Do NOT attempt to sync ambient RNG** (global `rand()` can't be
seed-synced — shared stream + divergent call counts + no asset edits; the user's
established conclusion). Note: client ambient spawners should be **suppressed**
where the spawn is host-authoritative (so only the host's detected output is
mirrored), or left peer-symmetric where camera-relative (the firefly call).

### One-line architecture

> **Scripted events:** host clock is the roller; suppress the client's
> `eventer->runEvent`; host diffs `saveSlot.passEvents` each minute-pulse and
> broadcasts `EventFire{rowName, special}`; client replays via reflected
> `eventer->runEvent`; sync `passEvents`/`allEvents` for dedupe (save-transfer +
> live). **Ambient tickers:** no central seam — per-species output detectors
> (firefly model). `activeEvents`/`setEvent` is state to mirror, not a roll.

---

## Appendix — key files / functions (all paths absolute-resolvable from repo root)

- **Event table:** `VotV/Content/main/datatables/list_events.uasset` →
  `research/bp_reflection/list_events.json` (69 `Fstruct_event` rows).
- **Event struct:** `…/CXXHeaderDump/struct_event.hpp` (FIntVector time + FName special).
- **Scheduler (deterministic clock-cross + `eventer->runEvent`):**
  `saveSlot::settime` — `research/bp_reflection/saveSlot.json` (function
  `settime`, region lines ~4131–4389). Signature `saveSlot.hpp:141`.
- **Persistent fired-set:** `saveSlot.allEvents` (0x00F0), `saveSlot.passEvents`
  (0x00C8), `saveSlot.specials` (0x0038). Boot dedupe: `mainGamemode` ubergraph
  `[1380-1406]`.
- **Dispatcher:** `Atrigger_eventer_C` (`trigger_eventer.hpp`) — `runEvent`
  (FName switch → `event_X->runTrigger`), `runSpecialEvent`, `summonArirPrank`.
  Disasm: `research/bp_reflection/trigger_eventer.json`.
- **Fire verb:** `AtriggerBase_C::runTrigger(Owner, Index)` (`triggerBase.hpp:111`).
- **Active-event refcount:** `lib_C::getEvent` / `setEvent` (`lib.hpp:112-113`),
  `mainGamemode.activeEvents`/`activeEvents_senders`/`disableEventTrack`
  (0x0E68/0x0E70/0x0E98). Music gate: `mainGamemode` ubergraph `[3027-3032]`.
- **Ambient ticker family:** `…/CXXHeaderDump/ticker_*.hpp` (33 classes,
  `: Aticker_base_C`). Representative rolls:
  `research/bp_reflection/ticker_mannequinSpawner.json` `[68-88]`,
  `ticker_fireflySpawner.json` `[5-24]`.
- **daynightCycle per-hour ambient rolls:**
  `research/bp_reflection/daynightCycle.json` ubergraph `[283-344]`; clock
  field `daynightCycle.timeZ` (0x02D0); `func_newHour`, `getNamedTime`.
- **`ariralRepEventHandler`** (NOT a roller): a reputation-threshold notifier —
  `ReceiveTick→calcRep`, on change iterates `int_globalChange` actors and calls
  `reputationChanged(prevRep)` (`ariralRepEventHandler.json`). Relevant only if
  reputation drives event eligibility; it does not pick/roll events.
- **`grayEventController`** (NOT a roller): a localized box-trigger that
  `Spawn()/Despawn()` grays on overlap (`grayEventController.json`).
- **Dev launcher (confirms single entry):** `Uui_eventRun_C` — `ui_eventRun.json`
  `ExecuteUbergraph [6]` `eventer->runEvent(name, row.special)`.

### Already coop-synced (noted DONE, not re-cataloged)
weather (v43), lightning, fireflies (v51 — host-detect-PSC-diff + broadcast, the
ambient-detector precedent for §5B), wind gusts (v50), sky/stars (v44),
time/clock (v36 — the **foundation for §5A**: the authoritative clock is the
scripted-event roller).
