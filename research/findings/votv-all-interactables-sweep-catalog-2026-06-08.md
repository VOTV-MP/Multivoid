# VOTV — complete catalog of hand-operated physical interactables (sweep-sync) — 2026-06-08

**Goal:** stop discovering desynced switches one at a time. Enumerate EVERY hand-
operated physical interactable (buttons, levers, switches, valves, knobs, breakers,
panels…) that carries a replicable binary/enum state, so we can sweep-sync them all
through the existing generic keyed-interactable engine (`coop/interactable_sync` +
a tiny `ue_wrap` wrapper + an `Adapter`). All evidence is cited from the standalone
SDK CXX header dump (`Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/`,
RULE 3 dev tool) — class, offset, type, and activation/apply verb per claim.

**TL;DR counts**

- **DONE (already synced):** 5 channels — doors (+ all `Adoor_C` subclasses, auto-
  covered by the descendant `IsDoor`), light switches, container lids, keypad, garage.
- **MISSING, in-scope for the easy sweep (clean keyed binary toggle):** **8 classes**
  → coverable by **2 generic family adapters** (one for the `AtriggerBase_C` button/
  lever/switch family, one for the `Aactor_save_C` appliance/valve family) + **1 small
  per-class wrapper** for the wire switch — i.e. **~3 adapters / 2 new ReliableKinds**,
  not 8 hand-written channels.
- **OUT-OF-SCOPE (compound machine / analog / minigame / UI / one-shot):** ~15
  classes documented at the end with the reason and a future path.

The next free `ReliableKind` is **35** (33=GarageDoorState, 34=SkyState are taken;
32 is reserved for the deferred GrimeDestroy — `protocol.h:409-411,813,823`). The
brief's "33/34 just taken, 32 reserved, next free 35" is correct.

---

## UPDATE (2026-06-08) — bytecode RE CLOSES the sweep: the 3 stragglers (§2C/2D/2E) are NOT keyed-toggle-syncable

A `bp_reflect` pass (read each class's Kismet bytecode in full) byte-verified that ALL THREE remaining
"MISSING in-scope" candidates fail the keyed-bool-channel contract — each for a concrete, different
reason. **They are SKIPPED; the keyed-toggle sweep is COMPLETE** with the shipped appliances
(`ApplianceState=35`) + power panel (`PowerControlState=36`). Building any of them would be a RULE-1
crutch (a known-broken sync). This UPDATE supersedes the optimistic "MISSING — in-scope" flags on the
§2C/2D/2E rows. Commands: `bp_reflect.py coordRadarDish prop_wireComponent_switch prop_radioNew prop`
+ `research/bp_reflection/_fn.py` / `_cfg.py` per function (all disassembled, none failed).

- **§2E `AcoordRadarDish_C::opened`@0x0410 — SKIP (wrong verb + not stateful + not saved).** Identity is
  fine (sKey, stable). BUT (1) the catalog's apply verb `moveLever` is the FUSE-PUZZLE lever (plays
  `leverTL` → `updPuzzle()`), NOT the deploy — it never reads/writes `opened`. (2) The deploy/retract is
  a one-shot SKELETAL MONTAGE played in the `actionOptionIndex` retract branch; `opened` flips on the
  montage's `finish` anim-notify. There is NO per-tick refresh verb that re-poses the dish from `opened`.
  (3) `opened` is NOT persisted (`getData`/`loadData` save only `fuses`+`isBroken`) → it resets to
  retracted on load. → Syncing it needs a MONTAGE-TRIGGER sync (replay open/close on the receiver), not
  the keyed-bool channel; low value (cosmetic + unsaved) → defer with the dish puzzle (§3).

- **§2C `Aprop_wireComponent_switch_C::Active`@0x0380 — SKIP (unstable player-built pKey).** It is one of
  ~40 `prop_wireComponent_*` logic components — the player-BUILT electronics/circuit kit. Identity =
  `Aprop_C::Key`@0x02E0, and a fresh player-placed prop is assigned `Key = NewGuid` per-peer at
  `Aprop_C::Init` (proven in `votv-storage-container-spawn-RE-2026-05-25.md`) → host and client keys
  DIVERGE → keyed sync cannot converge (the chipPile root cause). Also no single refresh verb (an inline
  `Billboard.SetSprite` ubergraph block + the pull-based `wirePass(wire)`, where a wire asks the switch
  "does current pass?" — only propagates when `active`). → The wiring subsystem (components + the
  `Awire_C` graph keyed by `key_A`/`key_B` endpoint strings) needs its OWN spawn-graph replication (like
  the prop pipeline), NOT this channel. SKIP from the sweep.

- **§2D `Aprop_radioNew_C::A_0`@0x0378 — SKIP (dead state field + unstable pKey).** `A_0` is referenced
  ZERO times in the radio's bytecode — it is never read or written by any radio logic (a vestigial
  field). `player_use` → immediate RETURN (no-op); `openMedia(url)` → `MediaSound.Play()` unconditionally,
  ignoring `A_0`. So there is no working on/off bool to poll/apply. Identity is also unstable (the radio
  inherits `Aprop_C::canPickup` → a carryable prop → per-peer NewGuid key). → SKIP; the radio's real
  audio on/off isn't in this class. A future radio-audio sync is a separate media-sync, not a keyed toggle.

**Net:** the "physical switch/valve/breaker" desync class is FULLY closed by the shipped
DoorState/Light/Container/Garage/Keypad + Appliance(35) + PowerControl(36). The 3 stragglers belong to
OTHER subsystems (montage-trigger sync, wire-build replication, media sync) — none is a keyed toggle.
Verify-don't-guess (RULE 1) prevented 3 broken builds: a dead-field write, a chipPile-style desync, and a
wrong-verb no-op.

---

## 0. How the existing engine works (the extension contract)

`coop/interactable_sync.cpp` is ONE generic `Channel` driving N features. Each feature
is just an **`Adapter`** vtable over a `ue_wrap` engine wrapper + a file-static `Channel`
instance:

```cpp
struct Adapter {
    const char* name;  coop::net::ReliableKind kind;
    bool (*EnsureResolved)();
    bool (*IsInstance)(void* obj);            // class-descendant check
    std::wstring (*GetKey)(void* actor);      // cross-peer-stable Key string
    bool (*ReadState)(void* actor, bool& on); // poll the state field
    bool (*ApplyState)(void* actor, bool on); // drive to target (echo-suppressed)
    // HostAuth-only hooks (null for Symmetric): SuppressAutonomy/RestoreAutonomy/
    // RequestApply/SuppressHeld/ReleaseHeld/CanOpen
};
```

The **sender is a per-tick STATE POLL** (`Channel::PollAndBroadcast`), NOT a UFunction
observer — IDA-proven 2026-06-04 that the interaction verbs (`doorOpen`, `SetActive`,
`Open`, `runTrigger`, `acivae`, `player_use`/`actionOptionIndex`…) dispatch BP-internally
via `CallFunction → ProcessInternal` and bypass our `ProcessEvent` detour, so a POST
observer never fires. **Every interactable in this catalog is therefore POLLED on its
state field** (the brief's expectation is confirmed across the board). The receiver
resolves by Key, idempotently applies, primes the poll baseline (no echo), and on connect
the host snapshots the full state to a joiner.

**Two modes** (`Channel::Mode`):
- **Symmetric** — for state that is **inert once set** (no per-tick auto-revert): lights,
  containers, garage. Each peer is authoritative over the change it causes; the host
  relays a client edge. Reuses the `Channel` verbatim, all HostAuth hooks `nullptr`.
- **HostAuth** — for state whose **local sim re-drives it every tick** (a sensor /
  autoclose / `ReceiveTick`): doors only. Hold-register + `DoorOpenRequest` + autonomy
  suppression. **Auto-revert is the single discriminator that decides the mode.**

**Two identity schemes (both save-persistent FName Keys, deterministic, cross-peer
stable — proven for doors/garage):**
- `AtriggerBase_C::Key` @ **0x0260** — the trigger family (door/garage/lightswitch/
  button/lever/powerControl…). `triggerBase.hpp:11`.
- `Aactor_save_C::Key` @ **0x0230** — the save-actor family (faucet/sink/server/generator/
  shower/dish/kitchen/cremator…). `actor_save.hpp:8`. NOTE: `FindPropertyOffset` does NOT
  climb to a super, so resolve `Key` against `triggerBase_C` / `actor_save_C` directly —
  the same gotcha `ue_wrap/garage.cpp:37-47` already handles.
- `Aprop_C::Key` — the prop family (wire switch, radio). `ue_wrap::prop::GetKeyString`
  already exists (used by the swinger/container channel). VOTV props get a save-assigned
  Key via the same save system, BUT a freely-spawnable prop's Key can be a runtime GUID;
  a **level-placed** wire switch is stable. (Flagged per item.)

**MTA precedent we follow:** the keyed-interactable channel IS MTA's single-syncer /
state-broadcast pattern. Symmetric = "broadcast the resulting boolean state, receiver
applies idempotently" (`CClientGame` value sync — one state source, inert → last-writer-
wins converges with no oscillation). HostAuth = the full single-syncer override + hold
register (`CUnoccupiedVehicleSync` shape), reserved for entities whose local sim fights
the applied state (doors). This is the same precedent cited by the door + garage RE docs
and `memory/feedback_follow_mta_architecture.md`.

---

## 1. THE CATALOG TABLE

Legend — **Identity**: `tKey`=`AtriggerBase_C::Key`@0x0260, `sKey`=`Aactor_save_C::Key`@0x0230,
`pKey`=`Aprop_C::Key`. **Mode**: Sym=Symmetric, HA=HostAuth. **Status**: DONE / MISSING
(in-scope) / OUT (+reason).

### 1A. The `AtriggerBase_C` family (Key @ 0x0260)

| Class | Header | State field (offset / type) | Activation verb | Apply verb | Identity | Auto-revert? | Mode | Status |
|---|---|---|---|---|---|---|---|---|
| `Adoor_C` (+`Adoor_pryable_C`) | `door.hpp`, `door_pryable.hpp` | `Active`/`isOpened` (door BP) | `actionOptionIndex`/`doorOpen` (BP-internal) | `SmartApply`/ForceOpen | tKey | YES (sensor+autoclose) | HA | **DONE** (subclasses auto-covered by descendant `IsDoor`) |
| `Atrigger_lightRoot_C` | `trigger_lightRoot.hpp` | `IsActive` (group on/off) | `SetActive` (BP-internal) | `SetActive(on)`/switch `use()` | tKey | no | Sym | **DONE** (LightState=10) |
| `Alightswitch_C` | `lightswitch.hpp` | `A` (switch flip) | `use()`/`player_use` (BP-internal) | `CallUse` | tKey | no | Sym | **DONE** (LightState=10, keyed at switch) |
| `Agarage_C` | `garage.hpp` | `Open` @0x02E8 / bool | `runTrigger→acivae` (BP-internal) | `settime(Open)` snap | tKey | no | Sym | **DONE** (GarageDoorState=33) |
| `ApasswordLock_C` | `passwordLock.hpp` | typed buffer + 3 bools | (own module) | (own module) | tKey | n/a | n/a | **DONE** (KeypadState=25, own `keypad_sync`) |
| **`ApowerControl_C`** | `powerControl.hpp` | **5 press bools** `press_coord`@0x0380 / `press_downl`@0x0381 / `press_play`@0x0382 / `press_calc`@0x0383 / `press_light`@0x0384 + `Disabled`@0x03E8 | `actionOptionIndex`/`player_use` → `powerChanged(5 bools)` | `powerChanged(...)` (5-bool setter) | tKey | no (latched until re-pressed) | Sym | **MISSING — in-scope (multi-bool)** — the BASE POWER PANEL (5 system breakers: coords/downloading/playing/calc/light). Carries a 5-bit enum; needs a 5-bool payload OR 5 sub-keys. See §2B. **HIGH VALUE.** |
| **`Atrigger_button_C`** | `trigger_button.hpp` | NONE of its own (`Label`@0x02A0 is a display flag) | `actionOptionIndex` → `runAll(-1)` (BP-internal) | n/a (fan-out only) | tKey | no | — | **OUT (no own state)** — pure fan-out to `Objects[]`; we sync the TARGET (e.g. the garage's `Open`), which the poll already catches. Do NOT sync the button. |
| **`Abuttons_C`** | `buttons.hpp` | NONE (`Box`/`SkeletalMesh` only; animated press) | `actionOptionIndex`/`player_use` (BP-internal) | n/a (fan-out only) | tKey | no | — | **OUT (no own state)** — same as `trigger_button_C` but with a skeletal press anim; sync the target, not the button. (If a visibly-latched lit button is ever reported, its lit state lives on the target.) |
| `AceilingLamp_C` (+`AlightOut_ceil/floor/wall_C`) | `ceilingLamp.hpp`, `lightOut_*.hpp` | (driven by lightRoot) | — (lamp is an `Objects[]` target) | — | tKey | no | — | **OUT (slave of lightRoot)** — a lamp is fanned out by its `Atrigger_lightRoot_C`; syncing the root already drives it (the RE design's whole point). |
| `AambientLight_C` | `ambientLight.hpp` | (driven by lightRoot) | — | — | tKey | no | — | **OUT (slave of lightRoot)** — same as ceilingLamp. |
| `Agearer_C` | `gearer.hpp` | none (decorative spinning gears) | `runTrigger` (auto, from a target) | — | tKey | continuous | — | **OUT (cosmetic, no toggle)** — animated gears fired by a trigger; no player toggle, no replicable binary. |
| `Atrigger_alarm_C`, `AtriggerTimer_C`, `Atrigger_delay_C`, `Atrigger_box_C`, `Atrigger_eventer_C`, `Atrigger_spawnProp_C`, `Atrigger_sound_C`, `Atrigger_ambientSound_C`, `Atrigger_notif_C`, `Atrigger_achievement_C`, `Atrigger_jamDoor_C`, `Atrigger_forceObject_C`, `Atrigger_agrav_C`, `Atrigger_destroyByKeys_C`, `Atrigger_destroyInRadius_C`, `AariralNoTaker_C`, `Atrigger_arirEgg_C`, `Atrigger_breakDish_C`, `Atrigger_bedEvent_C`, … (~35 more) | (respective `.hpp`) | varies/none | `runTrigger`/`setActiveTrigger` from **overlap or script**, NOT a player E-press | — | tKey | n/a | — | **OUT (auto-fire volume triggers, not hand-operated)** — these are the bulk of the `AtriggerBase_C` subclass list (47 total). They fire from `OnComponentBeginOverlap` / scripts / events, carry no player-toggled binary state, and are not physical buttons. Explicitly excluded. |

### 1B. The `Aactor_save_C` family (Key @ 0x0230)

| Class | Header | State field (offset / type) | Activation verb | Apply verb | Identity | Auto-revert? | Mode | Status |
|---|---|---|---|---|---|---|---|---|
| **`Afaucet_C`** | `faucet_DUPL_1.hpp` | **`turnon`@0x0278 / bool** (+`Active`@0x0279) | `player_use` → `upd()` (BP-internal) | `upd()` / direct-write `turnon` then `upd()` | sKey | no | Sym | **MISSING — in-scope** — wall water valve. Clean bool. Has `ReceiveTick` (water effect) but it doesn't re-drive `turnon` (no autoclose). |
| **`Asink_C`** | `sink_DUPL_1.hpp` | **`isOn`@0x0278 / bool** | `player_use` → `updIsOn()`/`upd()` (BP-internal) | `updIsOn()` / direct-write+`upd()` | sKey | no | Sym | **MISSING — in-scope** — sink tap on/off. Clean bool. (`clean`@0x0268 float is the sponge-clean accumulator — out of this toggle.) |
| **`Aprop_shower_C`** | `prop_shower.hpp` | **`running_cold`@0x0298 / bool** (+`useType`@0x0299) | `player_use` → `upd()` (BP-internal) | `upd()` / direct-write+`upd()` | sKey | no (Tick = water FX only) | Sym | **MISSING — in-scope** — shower water on/off. Clean bool. (`clean`@0x0288 float = sponge accumulator, out.) |
| **`Akitchen_C`** | `kitchen.hpp` | **`Active`@0x02E1 / bool** (oven on) (+`powered`@0x02E2, `fixed`@0x02E0) | `player_use`/switch boxes → `upd()` (BP-internal) | `upd()` / direct-write+`upd()` | sKey | no | Sym | **MISSING — in-scope (oven toggle)** — the oven on/off (`Active`). The 6 counter/shelf child-actor doors are `Adoor_C` → already DoorState-synced. The `power` float ramp is cosmetic. |
| **`AserverBox_C`** | `serverBox.hpp` | **`Active`@0x03D5 / bool** (+`calc`@0x03D6) | `player_use`/breaker → `SetActive(bool)` (BP-internal) | **`SetActive(bool)`** (named setter!) | sKey | no | Sym | **MISSING — in-scope** — server power. Has a dedicated `SetActive(bNewActive)` UFunction (clean apply verb). `Aprop_serverBreaker_C` is just the lever that toggles this — sync the serverBox, not the breaker. |
| `AcoordRadarDish_C` | `coordRadarDish.hpp` | `opened`@0x0410 / bool (retract lever) + `IsBroken`@0x035C + fuse minigame | `actionOptionIndex` → `moveLever(bool)` (BP-internal) | `moveLever(bool)` | sKey | no | Sym | **MISSING — in-scope (the retract/deploy toggle ONLY)** — the deploy/retract lever (`opened`) is a clean hand toggle. The **fuse puzzle (`fuses`/`puzzleLights`) and `IsBroken` are OUT** (compound minigame, §3). Sync `opened` only; defer the puzzle. |
| **`Aprop_radioNew_C`** (`: Aprop_C`) | `prop_radioNew.hpp` | **`A_0`@0x0378 / bool** (radio on) | `player_use` → `openMedia`/ubergraph (BP-internal) | direct-write `A_0` + `upd`-equivalent (verify verb) | pKey | no | Sym | **MISSING — in-scope (if level-placed)** — radio power. pKey identity: stable if the radio is a fixed base prop; a player-spawned radio gets a runtime GUID (flag — verify Key stability in smoke, like the swinger/container channel's keysHash check). |
| `Awallunit_tapes_C` | `wallunit_tapes.hpp` | `Active`@0x0290 / bool (reel running) | `player_use` → `upd()` (BP-internal) | `upd()` / direct-write+`upd()` | sKey | no (Tick = reel anim) | Sym | **MISSING — in-scope (low priority)** — tape-reel machine running on/off (`Active`). Clean bool; reels are float (cosmetic). Niche; include in the sweep if cheap. |
| `Afire_C`, `Acampfire_C`, `Acandle_C`, `Arockcandle_C` | `fire_DUPL_1.hpp`, `campfire.hpp`, `candle.hpp`, `rockcandle.hpp` | lit-ness implicit (`Time`/fuel float, particle), no clean bool | `attemptIgnite`/`ignite(fuel)` | `ignite`/`extinguishFire` | sKey | burns down (Tick) | — | **OUT (no binary toggle; ignite-mechanic)** — lit state is a fuel/particle continuum driven by ignite/extinguish + burn-down, not a player on/off latch. (Could be a future "lit bool" sync, but it is not a switch/lever; out of THIS sweep.) |
| `Aatm_C` | `atm.hpp` | `isBusy`@0x0291 (transient in-use) | `player_use` → opens `Uui_atm_C` | — | sKey | n/a | — | **OUT (UI machine)** — opens a withdrawal UI; no persistent player-toggled physical state. (Money flow is economy, synced elsewhere.) |
| `Apanel_radar_C` | `panel_radar.hpp` | only `lookAt*`/`lookingAt*` hover flags + module slots | `player_use` → opens `Uui_radar_C` | — | sKey | n/a | — | **OUT (UI/module console)** — module-slot console; the lever opens a screen UI. No binary toggle. (Module insertion is item state, separate.) |
| `Apanel_SATconsole_C` | `panel_SATconsole.hpp` | `Root`@0x02B0, `isLlamable`@0x02B1 (config flags) | `player_use` → `Uui_console_C` (controls a dish) | — | sKey | n/a | — | **OUT (UI control surface)** — drives a `Adish_C`; no player on/off toggle of its own. |
| `Adish_C` | `dish.hpp` | `isMoving`@0x0384, `isRotate`@0x03A0, rotation/`calibration` (continuous) | (driven by SAT console) | continuous aim | sKey | continuous (Tick aims) | — | **OUT (analog aim)** — satellite-dish pointing is continuous rotation, not a binary toggle. Future: a pose-stream/aim-sync (separate, like ATV), NOT the keyed channel. |
| `Aradiotower_C` | `radiotower.hpp` | `IsBroken`@0x03D0 + fuse/switch minigame (`puzzleData`/`fuses`) | `actionOptionIndex` (puzzle) | `setBroken(bool,bool)` | sKey | no | — | **OUT (compound minigame)** — fix-the-tower puzzle; `IsBroken` is the OUTCOME of a multi-element minigame. Sync the minigame as a unit later (§3), not as a single toggle. |
| `Acremator_C` | `cremator.hpp` | `IsClosed`@0x0399 / bool (door) + `IsActive`@0x0370 (burning) + `pouring`@0x0388 | `useLever`/`setClosed(bool)`/`openDoor` (BP-internal) | `setClosed(bool)` / `openDoor` | sKey | partial (burn Tick) | — | **OUT (compound machine) — borderline** — the `IsClosed` swinger door + `IsActive` burn are coupled to a gascan-pour + fireball sequence. The door alone could be a future toggle, but the burn cycle isn't binary. Defer (§3). |
| `AcargoLift_C` | `cargoLift.hpp` | `moving`@0x02C0, `dir`@0x02C1, `Alpha`@0x02C8 (timeline position) | `player_use` (call buttons) → timeline | continuous move | sKey | continuous (Tick moves) | — | **OUT (moving platform / analog)** — an elevator whose synced state is a continuous POSITION (like a vehicle), not a binary toggle. Future: a position pose-stream, NOT the keyed channel. |
| `Awindturbine_C` | `windturbine.hpp` | `doRot`/`doSpin` (auto), rotation continuous | overlap (`BndEvt` boundCaps), NO `player_use` | — | sKey | continuous | — | **OUT (ambient, not hand-operated)** — spins from wind/overlap; no player toggle. |
| `AgroundHose_C` | `groundHose.hpp` | `connToFauc`@0x0300, `moving`@0x02F0 (spline deploy) | `player_use` (lay hose) → spline | continuous spline | sKey | continuous | — | **OUT (spline/continuous deploy)** — hose laying is a continuous spline state, not a binary toggle. |
| `Awallunit_console_C` | `wallunit_console.hpp` | NONE (mesh only — empty class) | — | — | sKey | — | — | **OUT (no state / not interactive)** — empty body, just a `StaticMesh`. |
| `Atoilet_C` | `toilet_DUPL_1.hpp` | `used`@0x0288 / bool (flush) | `player_use` → flush (BP-internal) | — | sKey | momentary (auto-resets) | — | **OUT (momentary one-shot)** — flush is a transient action that auto-resets; not a held toggle. (Sync-able as a one-shot event later if ever wanted; trivial, low value.) |
| ~80 other `Aactor_save_C` subclasses (spawners, event controllers, decals, drains, grates, souls, dreamers, mannequins, obelisks, hex pillars, batch/blackball/bunny/piramid/ufo spawners, `event_*`, `cord`/`cordSocket`/`wire`/`weld`/`nail`/`hook`, `customWall`, `poster`/`sticker`/`rug`, `growingPlant`, `saltpile`, `treehouse`, `outsideChurch`, …) | (respective `.hpp`) | varies/none | mostly NOT a binary player-toggle (spawn/event/decal/construction) | — | sKey | varies | — | **OUT (not a hand-operated binary switch)** — the large remainder of the `Aactor_save_C` family is spawners, story/event controllers, decals, and construction/attachment props. None is a physical button/lever/switch with a replicable on/off state. Several (cord/wire/weld/hook, growingPlant) are their own gameplay subsystems (cabling, farming) for separate RE, NOT this sweep. |

### 1C. The `Aprop_wireComponent_C` electrical family (Key @ pKey)

| Class | Header | State field (offset / type) | Activation verb | Apply verb | Identity | Auto-revert? | Mode | Status |
|---|---|---|---|---|---|---|---|---|
| **`Aprop_wireComponent_switch_C`** | `prop_wireComponent_switch.hpp` | **`Active`@0x0380 / bool** | `actionOptionIndex` → `wirePass` (BP-internal) | direct-write `Active` (+ `wirePass` to propagate) | pKey | no | Sym | **MISSING — in-scope** — the wire-circuit toggle switch (electrical wiring puzzle). Clean bool. pKey identity (verify stability — wire components are placed but check the keysHash). |
| `Aprop_wireComponent_button_C` | `prop_wireComponent_button.hpp` | `used`@0x0398 / bool (momentary) + `wire`@0x0380 | `actionOptionIndex` → `wireConnected` | n/a (momentary fan-out) | pKey | momentary | — | **OUT (momentary button, no held state)** — fires a wire pulse; `used` is transient. Sync the wire/target state, not the button. |
| `Aprop_serverBreaker_C` | `prop_serverBreaker.hpp` | NONE (empty body) | `playerHandUse_RMB` (BP-internal) | n/a (fan-out) | pKey | no | — | **OUT (no own state — toggles serverBox)** — the breaker lever flips its parent `AserverBox_C::Active`; sync the serverBox (already in §1B), not the breaker. The user's "breaker box" → covered via serverBox. |

---

## 2. IMPLEMENTATION PLAN for the MISSING in-scope set

**In-scope MISSING (8):** `Afaucet_C`, `Asink_C`, `Aprop_shower_C`, `Akitchen_C`(oven),
`AserverBox_C`, `Aprop_radioNew_C`, `Awallunit_tapes_C`, `Aprop_wireComponent_switch_C`
— all **single-bool Symmetric toggles** — PLUS one multi-bool: `ApowerControl_C` (the base
power panel), and one partial: `AcoordRadarDish_C::opened` (deploy lever).

The single-bool ones DO NOT each need a bespoke channel. Group them.

### 2A. ONE generic "save-actor toggle" adapter family (covers 6 classes with 1 ReliableKind)

`Afaucet_C` / `Asink_C` / `Aprop_shower_C` / `Akitchen_C` / `AserverBox_C` /
`Awallunit_tapes_C` are all `Aactor_save_C` descendants keyed by `sKey`@0x0230 with a
single bool toggle and a `upd()`-style refresh. Build **one** wrapper + **one** adapter
whose `IsInstance` accepts the whole family and whose ReadState/ApplyState dispatch by
class to the right field offset + refresh verb:

- **New wrapper:** `ue_wrap/save_toggle.{h,cpp}` — resolves each class's UClass + its bool
  offset + its refresh verb at `EnsureResolved` (a small static table):
  | class | bool field | offset | refresh/apply verb |
  |---|---|---|---|
  | `faucet_C` | `turnon` | 0x0278 | `upd` |
  | `sink_C` | `isOn` | 0x0278 | `updIsOn` (then `upd`) |
  | `prop_shower_C` | `running_cold` | 0x0298 | `upd` |
  | `kitchen_C` | `Active` | 0x02E1 | `upd` |
  | `serverBox_C` | `Active` | 0x03D5 | `SetActive` (named bool setter — preferred apply) |
  | `wallunit_tapes_C` | `Active` | 0x0290 | `upd` |
  - `IsInstance(obj)` = descendant of ANY of the 6 UClasses (one `IsDescendantOfAny` call).
  - `GetKey` = read `sKey`@0x0230 (resolve against `actor_save_C`).
  - `ReadState` = read the per-class bool.
  - `ApplyState` = for `serverBox_C` call `SetActive(on)`; otherwise direct-write the bool
    then call the class's refresh verb (`upd`/`updIsOn`) so the mesh/FX/audio repaint
    (mirrors the lights' `use()` "flip visual + fan out" lesson — a bare field write won't
    re-run the visual/audio side). **VERIFY in disasm** that `upd()` reads the field and
    drives the visuals (true for all six by name; confirm the faucet/sink/shower `upd`
    path the same way the garage `settime` body was confirmed — `bp_reflect.py faucet sink
    prop_shower kitchen serverBox wallunit_tapes` + `_cfg.py`).
- **New ReliableKind:** `ApplianceState = 35` (v45), reusing **`KeyedTogglePayload` (40 B)**
  verbatim — exactly like DoorState/Light/Container/Garage. Add to
  `IsClientRelayableReliableKind` (Symmetric → host relays a client edge).
- **Wire into `interactable_sync.cpp`:** one `g_applianceAdapter` + `Channel
  g_appliance{g_applianceAdapter};` (default Symmetric) + a `ChannelForKind` case + index
  in `IndexChannels` + the four one-line hooks (`Install`/`QueueConnectBroadcastForSlot`/
  `Tick`/`OnDisconnect`). **This is the catalogued "adapter + a few lines"** — 6 desynced
  appliances fixed by ONE adapter.

> **Design choice (RULE 1, follow MTA):** one family adapter, not six channels. MTA syncs a
> *category* of like entities through one syncer keyed by a stable id; the per-class field
> offset is the only thing that differs, so it belongs in the wrapper's resolve table, not
> in six copies of the channel. (Same reasoning the lights channel keys at the switch and
> fans out — one hook per family.)

### 2B. `ApowerControl_C` — the base power panel (5-bool, its own small handling)

The base's main power board carries **5 independent system toggles** (`press_coord`,
`press_downl`, `press_play`, `press_calc`, `press_light`) + `Disabled`. This is the
highest-value missing interactable (it gates the whole base's power) but it is NOT a single
bool. Two clean options:

- **Option A (preferred — reuse the generic engine):** emit **5 sub-keyed
  `KeyedTogglePayload`s** under a new `PowerControlState = 36` kind, with the wire key =
  `"<panelKey>#coord"` / `"#downl"` / `"#play"` / `"#calc"` / `"#light"`. The
  `save_toggle`/trigger adapter pattern handles each as an independent Symmetric bool; the
  receiver writes the bit and calls `powerChanged(...)` (the 5-bool setter at
  `powerControl.hpp:100`) with the full reconstructed 5-tuple. Identity = `tKey`@0x0260 +
  the suffix. No new payload struct; reuses `KeyedTogglePayload`. **Recommended** — it
  drops straight into the generic channel.
- **Option B:** a dedicated 5-bool `PowerPanelPayload` (`WireKey key` + 5×uint8) under
  `PowerControlState = 36`, applied in one `powerChanged(a_calc,a_downl,a_coords,a_play,
  a_light)` call. One packet/edge, but a new struct + bespoke apply.

Either way the **apply verb is `powerChanged(5 bools)`** (a real setter that fans out to
servers/doors/lightRoots — `setPowered`/`sendPower`), and the **poll reads the 5 press
bools**. Auto-revert: none (latched until re-pressed) → **Symmetric**. Do this as a small
follow-up after 2A (it is one panel, but it is THE panel). RE the exact `powerChanged`
parameter semantics (which bool = which subsystem, and whether `Disabled` gates) via
`bp_reflect.py powerControl` before shipping — the field names suggest the mapping but
confirm it byte-exact.

### 2C. `Aprop_wireComponent_switch_C` — wire switch (1 class, tiny)

`Active`@0x0380, keyed by `pKey`. Either fold into the appliance ReliableKind=35 (its
`IsInstance` then also accepts the wire switch and reads `pKey` instead of `sKey` — slightly
messier mixed-identity), OR give it its own one-class adapter under the same kind=35 with a
`pKey` GetKey. Cleanest: a **separate tiny `ue_wrap/wire_switch.{h,cpp}`** + its own adapter
sharing `KeyedTogglePayload`, because its identity (pKey) and apply (direct-write `Active` +
`wirePass` to propagate the circuit) differ from the save-actor family. Verify pKey
stability in smoke (keysHash host-vs-client) before relying on it.

### 2D. `Aprop_radioNew_C` — radio power (1 class, optional)

`A_0`@0x0378, `pKey`. Same shape as the wire switch (pKey, direct-write bool). Low priority
/ verify identity (player-spawned radios get GUIDs; only a fixed base radio is stable). Fold
into the wire-switch-style pKey adapter or skip until requested.

### 2E. `AcoordRadarDish_C::opened` — deploy/retract lever (partial, defer)

The dish-deploy lever (`opened`@0x0410, apply `moveLever(bool)`, `tKey`/`sKey`=sKey) IS a
clean Symmetric toggle and can join the appliance family (it's an `Aactor_save_C`). BUT the
same actor's **fuse minigame** is OUT (§3), so syncing `opened` in isolation is fine and
independent. Add it to the 2A resolve table as a 7th entry (`coordRadarDish_C` / `opened` /
0x0410 / `moveLever`) **if** hands-on shows the deploy desyncs; otherwise defer with the rest
of the dish puzzle.

### Suggested order

1. **2A appliance family** (faucet, sink, shower, kitchen-oven, serverBox, tapes) — one
   adapter, `ReliableKind::ApplianceState = 35`, biggest coverage-per-effort. **Do first.**
2. **2B powerControl** (`PowerControlState = 36`, 5 sub-keys) — highest single value (base
   power), small once 2A's pattern exists.
3. **2C wire switch** + optionally **2D radio** — tiny, pKey, verify identity.
4. **2E coordRadarDish deploy lever** — only if reported; the dish puzzle stays deferred.

**Net:** the entire "physical switch/valve/breaker" desync class is closed by **2 new
ReliableKinds (35, 36) + ~3 adapters + 2–3 small wrappers**, all on the existing generic
channel, all Symmetric (none auto-reverts — only doors do), all reusing `KeyedTogglePayload`
except optionally powerControl Option B. No new identity scheme (sKey/tKey/pKey all already
have read helpers).

---

## 3. OUT-OF-SCOPE bucket (compound / analog / minigame / UI / one-shot) — future, separate

Not part of the easy keyed-toggle sweep; each needs its own design (RE first), NOT the
generic channel:

- **Compound minigames** (multi-element solved state): `Aradiotower_C` (fuse+switch puzzle,
  `IsBroken` outcome), `AcoordRadarDish_C` fuse puzzle, `AtransformerMGPanel_C` (the
  generator's sine/rotator/switch color-grid minigame — `switches_states`/`rotators_states`
  TArrays, cursor-`BndEvt` driven, NOT E-press). Sync as a *unit* (the full puzzle state
  array + completion) later, or just sync the OUTCOME (`IsBroken`/`fixed`) once and let each
  peer's local puzzle be cosmetic.
- **Continuous/analog position**: `Adish_C` (aim rotation), `AcargoLift_C` (elevator
  position), `AgroundHose_C` (spline deploy), `Awindturbine_C`/`Agearer_C` (ambient spin),
  conveyors (`Aconveyor_2x2_C::dir`/`Speed` — belt motion; `dir` is technically a bool but
  the belt is continuous-motion ambient). These want a pose/value STREAM (like the ATV/
  kerfur pose work), not a keyed binary toggle.
- **UI machines** (open a widget, no persistent physical toggle): `Aatm_C`, `Apanel_radar_C`,
  `Apanel_SATconsole_C`, `Alaptop_C`, `Atelescope_C`/`AtelescopeMars_C`, `Adrone_C`/
  `AdroneConsole_C`. Their *effects* (economy, dish aim, drone) are synced by their own
  subsystems, not as a switch.
- **Ignite-mechanic lights** (fuel/particle continuum, not a player on/off latch):
  `Acandle_C`, `Afire_C`, `Acampfire_C`, `Arockcandle_C`, `Axmaslight_C`. A future "lit
  bool" sync is possible but they are not switches.
- **Momentary one-shots** (auto-reset, no held state): `Atoilet_C` (flush),
  `Aprop_wireComponent_button_C`/`Atrigger_button_C`/`Abuttons_C`/`Aprop_serverBreaker_C`
  (pure fan-out — we sync the TARGET they actuate, never the button itself).
- **Auto-fire volume triggers** (the ~35 remaining `AtriggerBase_C` subclasses): fire from
  overlap/script, not a hand E-press; carry no player-toggled binary. Not interactables.

---

## 4. PROVENANCE

- **SDK (RULE 3 dev tool):** `Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/`
  — `triggerBase.hpp`, `actor_save.hpp`, `door.hpp`, `door_pryable.hpp`, `garage.hpp`,
  `trigger_button.hpp`, `buttons.hpp`, `powerControl.hpp`, `lightswitch.hpp`,
  `trigger_lightRoot.hpp`, `ceilingLamp.hpp`, `lightOut_*.hpp`, `gearer.hpp`,
  `faucet_DUPL_1.hpp`, `sink_DUPL_1.hpp`, `prop_shower.hpp`, `kitchen.hpp`, `serverBox.hpp`,
  `wallunit_tapes.hpp`, `wallunit_console.hpp`, `coordRadarDish.hpp`, `dish.hpp`,
  `radiotower.hpp`, `cremator.hpp`, `cargoLift.hpp`, `windturbine.hpp`, `groundHose.hpp`,
  `candle.hpp`, `toilet_DUPL_1.hpp`, `atm.hpp`, `panel_radar.hpp`, `panel_SATconsole.hpp`,
  `conveyor_2x2.hpp`, `generator.hpp`, `transformerMGPanel.hpp`, `prop_radioNew.hpp`,
  `prop_wireComponent_switch.hpp`, `prop_wireComponent_button.hpp`, `prop_serverBreaker.hpp`.
  Class-family enumeration: `grep ': public AtriggerBase_C'` (47 hits), `': public
  Aactor_save_C'` (~100 hits), `'void player_use(class AmainPlayer_C'` (87 hits).
- **Existing pattern (extend this):** `coop/interactable_sync.{h,cpp}`,
  `ue_wrap/{door,lightswitch,swinger,garage,prop}.{h,cpp}`, `coop/net/protocol.h`
  (`KeyedTogglePayload`@1243, `ReliableKind`@776-832, `kProtocolVersion=44`@415),
  `coop/net/session_lanes.h` (`IsClientRelayableReliableKind`),
  `coop/net/wire_key_util.h`.
- **Prior RE (templates):** `votv-doors-and-lightswitches-RE-2026-05-25.md`,
  `votv-garage-door-button-sync-RE-2026-06-08.md`,
  `votv-keypad-door-BP-disassembly-2026-06-06.md`. MTA precedent catalog:
  `memory/feedback_follow_mta_architecture.md`; `reference/mtasa-blue/` single-syncer.
- **To disassemble the apply verbs before shipping** (verify, don't guess — RULE 1):
  `tools/bp_reflect.py <asset> ...` + `research/bp_reflection/_cfg.py` (offset-aware CFG),
  as used for the garage `settime`/`runTrigger` byte-exact trace. Confirm each `upd()` /
  `updIsOn()` reads its bool and drives the visuals, and `powerChanged`'s 5-bool→subsystem
  mapping, the same way `Agarage_C::Open`@0x02E8 was confirmed an `EX_InstanceVariable`.

**Bottom line:** 5 done, **8 missing in-scope** (closeable by ~3 family adapters + 2 new
ReliableKinds 35/36, all Symmetric, all on the existing channel), ~15 explicitly out (with
reasons + future paths). No standalone `trigger_lever_C` exists — levers are components on
machines (`powerControl` lever_0..4, `radiotower`/`coordRadarDish` reset levers), and the
"breaker box" is `Aprop_serverBreaker_C` → synced via its `AserverBox_C::Active` target.
