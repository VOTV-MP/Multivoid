# ATV (quadbike) — full RE + coop sync status   (STATUS: **RE COMPLETE 2026-08-29** · sync **PARTIAL**: body pose only)

*[↑ vehicles index](README.md) · [↑ docs index](../README.md)*

The canonical doc for VOTV's ATV **and its upgrade system**. Supersedes the ATV sections of the
2026-06-08/06-15 point-in-time findings wherever they disagree — those stay as the record of what was
known then; **this file is the current truth**, and every correction is named.

> **THE SHIPPED SYNC IS A CRUTCH (USER RULING, 2026-08-29).** It is entry **C1** in
> [`docs/CRUTCHES.md`](../CRUTCHES.md) and is being redesigned, not extended. In one line: the mirror
> freezes a constraint rig whose entire purpose is suspension travel, then fails to freeze it cleanly
> — the wheels are never parked, steering and torque are tick-driven so both die, six of seven hit
> delegates still fire, and the transport is the reliable stream the 2026-06-08 blueprint labelled
> "acceptable only as a stopgap". Direction settled: a vehicle-sync subsystem in MTA's shape
> (always-simulating corrected mirrors + single-syncer election + unreliable-sequenced pose split from
> a reliable seed), because it **deletes the freeze/unfreeze state machine** where nearly every defect
> in this lane lives. §9 below remains an accurate description of what ships; read it as the
> as-built, not as the target.

**Evidence tags.** **[V]** measured 2026-08-29 from the cooked Blueprint bytecode / pak datatables
(file + offset cited) · **[V-src]** read from our own shipped source · **[RD]** reasoned from measured
facts, composition not run · **[A]** asserted by an earlier doc, not re-verified here · **[?]** unmeasured.

**Sources for every [V] claim below**
- `research/bp_reflection/ATV.json` — full UAssetAPI disassembly of `/Game/objects/ATV` (449 exports:
  1 ClassExport, **245 FunctionExports**, 203 NormalExports; **223 class properties**).
- `research/bp_reflection/ATV_cfg/ATV.txt` — offset-aware CFG of the ubergraph **and every named
  function** (18,085 lines). Ubergraph offsets are written `ub <off>`; function offsets `<fn>@<off>`.
- `research/pak_re/extracted/VotV/Content/main/enums/enum_physicalModules.{uasset,uexp}`.
- `research/pak_re/extracted/VotV/Content/main/datatables/list_store.uasset` (473 rows),
  `list_craftRecipes.uasset` (189 rows).
- `research/pak_re/extracted/VotV/Content/objects/prop_atvUpgrade_*.uasset` (13 leaf classes).
- Re-render any function with `python research/bp_reflection/_fn.py ATV <FunctionName>`.

---

## 0. TL;DR — what this RE changed

| # | Finding | Impact |
|---|---|---|
| 0.1 | **`modules[]` (a `TArray<enum_physicalModules>`) is the SINGLE source of truth for all 13 upgrades.** Every `has*` bool, every module mesh, the light cone, the top speed, the exhaust FX and the body physical-material are *derived* by one parameterless BP function, `updUpgrades()`. | Upgrade sync is **one array + one call**, not 13 lanes. |
| 0.2 | **`enum_atvUpgrades` is EMPTY** — it holds only `enum_atvUpgrades_MAX`. The live enum is `enum_physicalModules` (34 values, 13 of them ATV). | Do not build against `enum_atvUpgrades`; it is a dead asset. Closes an `[?]` in `docs/upgrades/README.md` §5. |
| 0.3 | **The ATV is NOT purchasable, but it IS runtime-spawnable.** All 473 `list_store` rows scanned: no row's `object` is `ATV_C`/`ATV_Child_C`, nor is it in `list_craftRecipes` — only its *parts* are buyable. **But `list_props` has a row `atv` whose `spawnAsObject` is `ATV_C`, `hidden=false`** [V], reached by `lib.PropToObject` → `spawnPropThroughGamemode` from `ui_spawnmenu`. | The "purchased ATV" premise behind the 2026-06-15 Gap-B design is **FALSE**, but the mechanism it built is **still required** — the shipped `AtvSpawn`/`AtvDestroy` synth-key lane is what covers a runtime ATV, which really can exist. **The RULE-2 deletion is CANCELLED; what is owed is a comment correction.** §11.4. |
| 0.4 | **`vehicleGetParts()` / `teleportVehicleAdvanced()` are a matched READ/WRITE pair for the FULL 4-body rig pose** (body + front-L + front-R + back-root, each loc+rot). The game ships exactly the primitive a correct vehicle mirror needs. | Our mirror moves **only the actor**; the wheels are separate constrained rigid bodies — §9.4, a defect candidate. |
| 0.5 | **The install trigger is INVISIBLE.** `mainPlayer` dispatches `playerUsedOn` via `EX_LocalVirtualFunction` [V, `mainPlayer.json`], and the whole install path lives inside `ExecuteUbergraph_ATV`. | Install cannot be hooked. It must be an **act-as-host INTENT naming an artifact** (the held `prop_atvUpgrade_C`) — `COOP_SYNCER_MODEL` §2b step 2, the `order_sync` shape. |
| 0.6 | **The ATV's inventory container has a DETERMINISTIC key**: `getDefaultContainerName()` = `atv_inventoryContainer\|<atv key>`. | The container is cross-peer addressable for free — no eid machinery. |
| 0.7 | The save round-trip carries **19 state slots** across 6 arrays; **`modules[]` is the only `bytes` entry**. Post-load derivation is one call: `processKeys()` = `createContainer(); updTires(); updSpareTire(); updDirt(); updUpgrades();`. | `processKeys()` is the single "re-derive everything from raw fields" seam a receiver needs. |
| 0.8 | **Three shipped wire bits are written and never read**: `stateBits` bit0 (`isDriven`), bit1 (`brake`), bit2 (`grabbed`). Only bit3 (`authored`) is consumed. | Dead wire surface today; the fields already exist for the design. |

---

## 1. Class + anatomy

`AATV_C : APawn` [V]. `ATV_Child_C : ATV_C` adds **no** properties — a pure placement variant [V].
**Not** a `UWheeledVehicleMovementComponent` vehicle: it is a hand-built physics rig.

### 1.1 The rig (why one transform is not the pose)

| Component | Type | Role |
|---|---|---|
| `mesh` | `UStaticMeshComponent` | **the actor ROOT**; the simulating body |
| `frontWheel_L` / `frontWheel_R` | `UStaticMeshComponent` | front wheels — **independent rigid bodies** |
| `backWheelRoot`, `backWheel_L`, `backWheel_R` | `UStaticMeshComponent` | rear axle assembly |
| `sus_FL1/FR1/BL1/BR1`, `ax_FL1/FR1/BL1/BR1` | `UPhysicsConstraintComponent` | suspension + axle constraints; drive torque is applied here |
| `spareTire` | `UStaticMeshComponent` | the spare, when `hasSpareTire` |
| `playerHit` | `UCapsuleComponent` | the **seat anchor** — the driver is placed at its world transform |
| `fuelbox`, `radiobox`, `spareTireBox`, `container`, `Box`, `Box1` | `UBoxComponent` | interaction volumes (refuel overlap, radio/container look-at) |
| `module_bigLights`, `module_front`, `module_solar`, `module_belt`, `module_container`, `module_radio`, `module_floaties`, `module_aircontrol` | `UStaticMeshComponent` | **the visible upgrade meshes** — visibility toggled by `updUpgrades` |
| `light_L`, `light_R` | `USpotLightComponent` | headlights; cone/intensity/colour changed by the bigLights module |
| `PointLight`, `PointLight1`, `backlights` | light / billboard | interior + brake lights; `backlights` visible iff `battery > 0` |
| `eff_carSmoke` | `UParticleSystemComponent` | damage smoke — driven **solely** by `updHealth()` |
| `eff_atvExhaust` | `UParticleSystemComponent` | exhaust; template swapped by the overcharged-engine module |
| `digitalMap`, `radio` | `UChildActorComponent` | the map screen and `prop_radio_atv_C` |
| `sitkerf` | `USkeletalMeshComponent` | the seated-kerfur passenger mesh |
| `Camera`, `lag`, `lagFl`, `lagRot` | camera + spring arms | driver camera |
| `tp` | `UBillboardComponent` | the explosion spawn anchor |

Because `mesh` **is** the actor root, `GetActorLocation/Rotation` == the body transform — but the
wheels are **not** its transform children; they are separate simulating bodies held by constraints.
The game acknowledges this: `teleportVehicle` (§2.6) re-places the wheels *after* moving the actor,
and `teleportVehicleAdvanced` takes four transforms.

### 1.2 Property census by subsystem (223 total) [V]

**Drive / engine** — `input_forward/back/left/right/alt/control`, `rotAlpha`, `torqAlpha`, `speed`,
`isDrive` (engine running), `isDrive_sound`, `turbo`, `nitro`, `speed_default`, `speed_turbo`,
`turnForce`, `exhaustForce`, `diff_fuel`, `interpTorque`, `interpVel`, `lastVel`, `mouseSteering`,
`invX`, `invY`, `deltaSeconds`.

**Consumables** — `fuel`, `empty`, `battery`, `energyWaste`, `health`, `brokenn`, `imp`, `dirt`.

**Occupancy** — `player` (`AmainPlayer_C*`), `prevPlayer`, `isDriven`, `hides` (`TArray<AActor*>`
hidden while seated), `playerUseOn`, `playersWheel` (`Aprop_atvWheel_C*` being held), `viewer`
(`AobjectViewer_C*`), `sittingKerfuro`, `allKerfuros`.

**Upgrades** — **`modules` (`TArray<TEnumAsByte<enum_physicalModules>>`)**, `upgradesNames`
(`TArray<FName>`), `selectUpgrades`, `upgradeUI` (`Uui_objectUpgrades_C*`), and the 13 derived bools
`hasBigLights hasBumper hasSolar hasBelt hasContainer hasGuns hasFloaties hasMap hasRadio
hasAircontrol hasFly hasChargedEngine has_alternator`.

**Tires** — `tires` (`TArray<bool>`, 4), `tiresDurability` (`TArray<float>`), `tiresDirt`
(`TArray<float>`), `tiresFixes` (`TArray<int>`), `tiresTypes` (`TArray<byte>`), `tirescount`,
`hasSpareTire`, `spareTire_durability`, `spareTire_dirt`, `spareTire_fixes`, `lookAtTire`,
`lookAtTireSocket`, `lookAtSpareTire`, `LookAtSpareTireBox`, `skipTireUpdate`.

**Dirt / decal** — `dirt`, `cleanVec`, `dirtVel`, `dirtVel_lerp`, `decal_dynmat`, `decalTexture`.

**Container** — `hasContainer`, `spawnedContainer` (`Aprop_inventoryContainer_atv_C*`), `containerKey`,
`lookAtContainer`.

**Flight / water / physics** — `fly`, `lift`, `airtime`, `isInAir`, `landed`, `canDoFlip`,
`isFrontflip`, `flipFinished`, `upVector`, `previousUpVector`, `prevDot`, `underwater`, `inWater`,
`floater`, `wheelsOnSurface` (`TArray<bool>`), `bodyIsOnTheGround`, `zapped`, `trap`.

**Identity / misc** — `key` (FName), `lastLoc`, `gamemode`, `april1st`, `timer`, `UnscrewProgress`,
`WidgetUnscrew`, `displaykey_*` (7 keybind display strings), `lights`, `brake`.

**CDO defaults** [V]: `fuel=100`, `health=100`, `battery=100`, `tires=[T,T,T,T]`,
`tiresDurability=[100,100,100,100]`, `tiresDirt=[0,0,0,0]`, `tiresFixes=[3,3,3,3]`,
`tiresTypes=[0,0,0,0]`, `speed_default=1600`, `speed_turbo=3200`, `turnForce=-100`.
The two speeds are **always** overwritten by `updUpgrades` (§4.4), so the CDO values hold only until
the first derivation.

---

## 2. The state machine (non-upgrade)

### 2.1 Seating — `playerSit` / `playerUnsit` [A 2026-06-08, unchanged]
`playerSit` (ub 5446→5616→7013): hide `hides[]`; `player := <mounter>`; `prevPlayer := player`;
`player.Capsule.SetMassScale(0)`; **`player.K2_AttachToActor(Self, SnapToTarget)`**; place the player
at `playerHit`'s world transform; `player.SetActorHiddenInGame(true)`;
**`GetPlayerController(0).Possess(Self)`** — which unpossesses `mainPlayer_C`, the discriminator the
mod relies on everywhere; attach `player.light_R` to `lagFl`; **`isDriven := true`**. Player side:
`player.atv := Self` (block 6755). `playerUnsit` (ub 7540) / `dismount` (ub 45915) reverse it and
clear `sittingKerfuro` / `allKerfuros`. `driven()` and `dismounted()` are empty stubs.

### 2.2 Fuel
Consumed **only in tick**, gated on `isDrive`: `fuel -= dt * (turbo ? 0.2 : 0.1) * diff_fuel`
[A, ub 37705/37715]. Refuel: a gas canister overlaps `fuelbox` → `fuelUp(gascan)` →
`gascan.getFuel(fuel /*by-ref*/, 100, …)`. The look-at gauge reads the **live field on demand**, not
in tick — so poking `fuel` on a tick-off mirror displays correctly for free.

### 2.3 Battery — the upgrade-parametrised drain [V, ub 33562-34460]
Recomputed every tick while `battery > 0`:

```
dt20        = deltaSeconds / 20
lightsCost  = dt20 / (hasBigLights   ? 5 : 2.5)      // big lights HALVE the light drain
turboCost   = dt20 / (has_alternator ? 6 : 4)
driveCost   = dt20 / 8
seatedCost  = dt20 / 10

a           = (isDriven ? seatedCost : 0)
b           = a + (isDrive ? driveCost : 0)
c           = has_alternator ? 0 : b                  // the ALTERNATOR zeroes the seat+engine draw
d           = c + (turbo ? turboCost : 0)
e           = d * (hasChargedEngine ? 2 : 1)          // the overcharged engine DOUBLES it
energyWaste = e + (lights ? lightsCost : 0)
```

`hasSolar` gates a `lib_obj` call in the same tick region (the solar recharge) [V, ub 43559-43569].
`updBattery()`: `backlights.SetVisibility(battery > 0)` then `Upd Lights()`.
`Upd Lights()`: if `battery <= 0` → `lights := false`; else set `light_R/L` + both point lights'
visibility from `lights`, and swap `module_bigLights`' material.

> The battery is the textbook `docs/COOP_WORLD_PROP_DIVERGENCE.md` shape — a local accumulator whose
> **rate is a function of the upgrade set**. On a mirror our `PrepareMirror` turns the actor tick off,
> so it does not diverge; it **stalls** (that doc's second symptom).

### 2.4 Health / damage / repair / explode
All damage paths converge on ub 19716: `health -= rawDamage * 2 * getBumperMult()`; then
`updHealth()`; then the explode test.
- `getBumperMult()` [V]: `hasBumper ? clamp(dot(normalize(impactNormal), GetActorForwardVector()), 0, 1) : 1.0`
  — the reinforced bumper scales **frontal** damage toward 0 at head-on.
- `updHealth()` is pure: `a = 1 - clamp(health,0,100)/100`; sets `eff_carSmoke`'s `freq` + `color`
  parameters and `Activate(true)`. Event-driven, never in tick → **callable on a tick-off mirror**.
- `runout()` [V]: `isDrive := false`, stops all drive audio + exhaust, `turbo := false`, clamps
  `fuel`/`health` to ≥ 0, sets `empty := fuel<=0`, `brokenn := health<=0`, then `updBattery()`.
- `isDown()` [V]: `fuel<=0 || health<=0 || underwater || battery<=0 || zapped`.
  `checkIfRunout()` calls `runout()` when `isDown()`.
- `toolboxFix()` [V]: if `floor(health) < 100` → `health := 100`, `brokenn := false`, `updHealth()`,
  `PlaySound2D(car_fix)`, return true. Full restore, never incremental.
  `toolboxCanFix() = health < 100`; `toolboxFixTime() = Lerp(15, 3, health/100)` seconds.
- `explode(fullBody)` → ub 23675: BeginDeferred-spawns `explosion_C` at `tp`, camera shake, ejects
  the driver. **It does NOT destroy the ATV** — it survives as a smoking wreck.
- `fire` / `ignite` / `fireDamage` / `extinguishFire` are empty stubs; smoke IS the entire damage VFX.

### 2.5 Tires (4 wheels + a spare) [V]
Per-wheel state is four **index-parallel arrays**: `tires[i]` (mounted), `tiresDurability[i]`,
`tiresDirt[i]`, `tiresFixes[i]` (int, the remaining "fix" count), plus `tiresTypes[i]` (mesh variant).

- `putTire(index, wheelObject)` — bounds-checks, refuses if `tires[index]` is already true (hint),
  else `tires[index] := true`, copies `durability` / `dirt` / `fixes` **off the `Aprop_atvWheel_C`**,
  `updTires()`, **`wheelObject.K2_DestroyActor()`**.
- `ejectWheel(index, component)` — BeginDeferred-spawns a `prop_atvWheel_C` at the tire socket with
  `durability`, `dirt` and **`fixes-1`**, FinishSpawning, inherits the socket's linear + angular
  velocity, `tires[index] := false`, `updTires()`, damage sound.
- `updTires()` — `setWheelsType()`, then per wheel: visibility + `SetCollisionEnabled` +
  `SetCollisionResponseToChannel` from `tires[i]`, the matching `tirePoint_*` collision, then
  **`BreakConstraint()` on all 8 constraints** and a full re-place of `sus_*` from
  `defaultTireLocations()` — a constraint rebuild.
- `damageWheel(index, damage, component)` → ub 15210. `processTire` decides damage-vs-dirt from
  `|impact| / mesh.GetMass()`: above threshold → `damageWheel`, below → dirt accumulation.
- `updSpareTire()` — `spareTire` visibility/collision from `hasSpareTire`; material from
  `lib_converters.getTireDamage(spareTire_fixes)`; `SetCustomPrimitiveDataFloat(0, spareTire_dirt)`;
  then `updDirt()`.
- `updDirt()` — early-returns unless `skipTireUpdate`; `mesh.SetCustomPrimitiveDataFloat(0, dirt)`
  and per wheel the damage material + dirt float. `diretTire(wheel)` accumulates `dirt += dirt/5000`
  (clamped 0..1) on both the wheel and the body when a downward line trace hits ground.
- `setWheelsType()` — if `april1st`, square wheels; else `wheelTypeToMesh(tiresTypes[i], i)` for
  i ∈ {0,1,2}. **Only three indices are read** [V] — index 3 has no `wheelTypeToMesh` call.
- `findTire(component) -> index` (Array_Find over the 4 wheel components);
  `getTire(index) -> component` (a 4-case switch). `regenConstraints()` is an **empty stub** [V].

### 2.6 Teleport — the matched rig-pose pair [V]
- `vehicleGetParts(out body_loc, body_rot, frontRight_loc/rot, frontLeft_loc/rot, back_loc/rot)` —
  reads `mesh`, `frontWheel_R`, `frontWheel_L`, `backWheelRoot` component-to-world transforms.
- `teleportVehicleAdvanced(body_loc, body_rot, fl_loc, fl_rot, fr_loc, fr_rot, back_loc, back_rot)` —
  `K2_SetWorldLocationAndRotation` on those same four components, `bTeleport=true`.
- `teleportVehicle(NewLocation, NewRotation)` — the simple form: `K2_SetActorLocation` +
  `K2_SetActorRotation`, **then** re-places `frontWheel_R/L` onto `ax_FR1`/`ax_FL1` and
  `backWheelRoot` onto `back`.

> Both forms exist **because moving the actor does not move the wheels.**

### 2.7 Water, flight, misc
`enterWater` / `leaveWater` / `enteredTheWater` / `exitTheWater` / `overlayBoyancy`;
`floater := hasFloaties` and the body's phys-material is swapped to `metal_barrel` when floating
[V, `updUpgrades` ub 2502-2551]. `underwater` feeds `isDown()`. `hasFly` gates a flight branch at
ub 25408; `hasAircontrol` gates `input_control` mid-air steering. `zapped` is set by
`reachedByLightning`. `padlock_lock` / `padlock_unlock` / `crowbarOpen` are ubergraph events
(44640 / 44639 / 44538); **no padlock field is saved** — only `trap` is.
`unscrewPanel` / `resetUnscrew` / `getUnscrewSpawn` drive `UnscrewProgress` + `WidgetUnscrew`.
`microwave` / `microwaveElec` / `addTemperature` are the standard prop-interface hooks.
`canPickup()`, `playerTryToHold()`, `canBePutInContainer()`, `canBeUsedHold()` all return **false**;
`getPriceMultiplier()` returns **0**; `skipRadial()` and `isButtonUsed()` return false [V].

---

## 3. The upgrade system — the enum, the props, the shop

### 3.1 `enum_physicalModules` — 34 values, 13 of them ATV [V]

`enum_atvUpgrades` exists as an asset but is **EMPTY** (only `enum_atvUpgrades_MAX`) — a dead enum.
The live one is `enum_physicalModules`; its `DisplayNameMap` is serialised in enumerator order, so the
34 `NewEnumerator<N>` keys map 1:1 to the invariant strings in the `.uexp`:

| id | display name | family |
|---:|---|---|
| 0 | `empty` | — |
| 1–7 | Global alert · Spectrogram visualisation · Automatic signal processing · Automatic polarity detection · Autosave signals · Storm filter · Keyboard remote control | workstation |
| **8** | **ATV big lights** | **ATV** |
| **9** | **ATV reinforced bumper** | **ATV** |
| **10** | **ATV solar panel** | **ATV** |
| **11** | **ATV belt** | **ATV** |
| **12** | **ATV container** | **ATV** |
| **13** | **ATV guns** | **ATV** |
| **14** | **ATV floaties** | **ATV** |
| **15** | **ATV map** | **ATV** |
| **16** | **ATV radio** | **ATV** |
| **17** | **ATV air control** | **ATV** |
| **18** | **ATV fly** | **ATV** |
| **19** | **ATV overcharged engine** | **ATV** |
| 20–32 | Lightning prediction · Log tape compression · Radar colors · Radar alarm · Radar radius · Radar path tracking · Radar radial search · Processing module LV1 / LV2 / LV3 · Coordinate auto rotation · Coordinate triangle visualiser · Hot swap | workstation / radar |
| **33** | **ATV alternator** | **ATV** |

The ATV set is **{8..19} ∪ {33}** — exactly 13, exactly matching the 13 `has*` bools.

### 3.2 The 13 upgrade props and their shop rows [V]

`prop_physModule_C : prop_C` carries a single `module` byte. `prop_atvUpgrade_C : prop_physModule_C`
adds **nothing** — it exists purely as the type discriminator the ATV casts to. The workstation
modules (`prop_physModule_autopol`, `_autosig`, `_lightning`, `_radarAlarm`, `_keyboardremote`,
`_coordTriRot`, `_coordTriVis`, `_autosavesig`) are siblings under `prop_physModule_C`, **not** under
`prop_atvUpgrade_C` — so the ATV's cast rejects them structurally.

| id | prop class | store row | price | in shop? |
|---:|---|---|---:|:--:|
| 8 | `prop_atvUpgrade_bigLights_C` | `atvup_lights` | 200 | yes |
| 9 | `prop_atvUpgrade_bumper_C` | `atvup_bumper` | 500 | yes |
| 10 | `prop_atvUpgrade_solar_C` | `atvup_solar` | 1500 | yes |
| 11 | `prop_atvUpgrade_belt_C` | `atvup_belt` | 500 | yes |
| 12 | `prop_atvUpgrade_container_C` | `atvup_container` | 350 | yes |
| 13 | `prop_atvUpgrade_guns_C` | *(none)* | — | **NO** |
| 14 | `prop_atvUpgrade_floaties_C` | `atvup_floaties` | 700 | yes |
| 15 | `prop_atvUpgrade_map_C` | `atvup_map` | 300 | yes |
| 16 | `prop_atvUpgrade_radio_C` | `atvup_radio` | 150 | yes |
| 17 | `prop_atvUpgrade_aircontrol_C` | `atvup_aircontrol` | 400 | yes |
| 18 | `prop_atvUpgrade_fly_C` | *(none)* | — | **NO** |
| 19 | `prop_atvUpgrade_overchargedEngine_C` | `atvup_chargedEngine` | 450 | yes |
| 33 | `prop_atvUpgrade_alternator_C` | `atvup_alternator` | 200 | yes |

All rows are category `enum_shopCats::NewEnumerator10`, subcategory **"Vehicle"**. The same category
also sells `atvwheel` → `prop_atvWheel_C` (200) and `atvcarbattery` → `prop_atvcarbattery_C` (200).
**`guns` and `fly` are not purchasable** — and `hasGuns` is read **nowhere** in `ATV.json` (its only
reference is the write in `updUpgrades`), so the guns effect, if any, lives outside the ATV class
[V; where, is **[?]**].

### 3.3 Craft [V]
`list_craftRecipes` (189 rows) contains no ATV. Only the wheel, both ways:
`atvWheel = 4× scrap_rubber + 1× scrap_metal`, and `atvWheelRubber` = `atvwheel → scrap_rubber`.

---

## 4. Install, remove, derive — the three code paths

### 4.1 INSTALL — hold the prop, use it on the ATV [V, ub 9411 → 9512 → 9713]

Inside the ATV's `playerUsedOn` handler:

```
cast<prop_atvUpgrade_C>(player.holding_actor)            -- ub 9411
  on failure -> try cast<prop_atvWheel_C>                -- ub 9950  (the putTire path)
  on success:
    if Array_Contains(modules, upgradeProp.module)       -- ub 9512
        lib.addHint("This upgrade is equipped")          -- ub 9608, and STOP
    else                                                 -- ub 9713
        Array_Add(modules, upgradeProp.module)
        updUpgrades()
        upgradeProp.K2_DestroyActor()
        PlaySoundAtLocation(drive_in, GetActorLocation())
```

Three properties that matter for sync: it is **idempotent by construction** (the `Array_Contains`
guard), it **consumes** the prop (destroy), and the module id is read **off the prop**, never chosen
by the player.

### 4.2 REMOVE — `takeOffUpgrade(player, name)` [V, `takeOffUpgrade@0..618`]

Reached from the upgrade UI: `upgradeTake(item)` → ub 45963 → `lib.getMainPlayer()` →
`takeOffUpgrade(mainPlayer, item)` → `viewer.genList()`.

```
i        = Array_Find(upgradesNames, name)
module   = modules[i]
actorCls = lib.physModToActor(module)                    -- module -> prop class
spawned  = BeginDeferredActorSpawnFromClass(actorCls, player.GetTransform())
           FinishSpawningActor(spawned, player.GetTransform())
player.HoldObject(false, spawned)                        -- the player now holds it
Array_RemoveItem(modules, modules[Array_Find(upgradesNames, name)])
updUpgrades()
```

`getUpgradesList(out items)` simply returns `upgradesNames` [V].

### 4.3 The parallel-array invariant
`updUpgrades()` rebuilds `upgradesNames` by walking `modules` in order:
`lib.physModToActor(modules[i])` → `lib.getPropNameFromClass(...)` → append. So `upgradesNames[i]`
names `modules[i]`. **Order is therefore cosmetic** (it is the UI list order), but keeping it
byte-identical across peers is free if the array is synced verbatim.

### 4.4 DERIVE — `updUpgrades()`, the one function that matters [V, 139 stmts / 4,092 bytes]

`selectUpgrades := false`; `upgradesNames` cleared and rebuilt (above); then **thirteen
`Array_Contains(modules, <id>)` tests**, each writing one bool plus its visual/physical consequences:

| id | bool | everything `updUpgrades` does with it |
|---:|---|---|
| 8 | `hasBigLights` | `module_bigLights` visible; `light_R/L` intensity `1.2` vs `0.9`; attenuation radius `15000`; colour `(1, .95, .8)` warm vs `(.8, .9, 1)` cool; inner cone `45°` vs `35°`; outer cone `65°` vs `55°` |
| 9 | `hasBumper` | `module_front` visible |
| 10 | `hasSolar` | `module_solar` visible |
| 11 | `hasBelt` | `module_belt` visible |
| 12 | `hasContainer` | `module_container` visible; `container` collision `QueryAndPhysics` vs `NoCollision`; `sitkerf` relative location moved |
| 13 | `hasGuns` | *(bool only — no other effect anywhere in `ATV.json`)* |
| 14 | `hasFloaties` | `module_floaties` visible; **`floater := hasFloaties`**; `mesh.SetPhysMaterialOverride(metal_barrel)` when floating |
| 15 | `hasMap` | `digitalMap.ChildActor.SetActorHiddenInGame(!hasMap)` |
| 16 | `hasRadio` | `module_radio` visible; `radiobox` collision `QueryOnly` vs `NoCollision`; **if `!hasRadio` → `radio.ChildActor.mediaPlayer.Pause()` + `.Close()`** |
| 17 | `hasAircontrol` | `module_aircontrol` visible |
| 18 | `hasFly` | *(bool only here; consumed at ub 25408)* |
| 19 | `hasChargedEngine` | **`speed_default := 2000` vs `1500`; `speed_turbo := 5000` vs `2250`**; `eff_atvExhaust.SetTemplate(eff_atvExhaustFire)` vs `eff_atvExhaust` |
| 33 | `has_alternator` | *(bool only here; consumed by the §2.3 battery drain)* |

`updUpgrades()` takes **no parameters, reads only `modules`, and touches no physics state** — the same
shape as `updHealth()`. That makes it safe to call on a `PrepareMirror`'d (tick-off, physics-off)
actor, exactly as `drone.cpp` and the v115 desk-audio lane already do with their BP calls.

### 4.5 What never fires
`intComs_stuffUpgraded(gamemode)` — the global "an upgrade happened" interface notify — resolves to
ub 28564, a bare `EX_PopExecutionFlow` [V]. **The ATV ignores it.** Do not build on it.

---

## 5. Save round-trip — the exact slot map [V]

`getData(out data)` builds an `Fstruct_save`; `loadData(data)` reads it back. Both disassembled in
full (`getData@0..968`, `loadData@0..3168`).

```
struct_save {
  class     = GetObjectClass(self)                    // ATV_C or ATV_Child_C
  transform = GetTransform()
  key       = key
  bools[0].bools = [ brake, lights, trap, hasSpareTire ]
  bools[1].bools = tires[]                            // 4
  floats[0].floats = [ fuel, health, battery, dirt, spareTire_durability, spareTire_dirt ]
  floats[1].floats = tiresDurability[]
  floats[2].floats = tiresDirt[]
  ints[0].ints     = tiresFixes[]
  ints[1].ints     = [ spareTire_fixes ]
  bytes[0].vectors_10 = modules[]                     // <-- THE UPGRADES; the only bytes entry
  names[0].vectors_11 = [ containerKey ]              // the only names entry
}
```

`loadData` restores all of the above and additionally:
- **`key := (data.key == None) ? 'atv' : data.key`** — the ATV *does* restore its key (unlike
  `kerfurOmega::loadData`, which drops it), so after one save round-trip a runtime ATV's key is
  deterministic [A 2026-06-15, consistent with this bytecode];
- derives `empty := fuel <= 0` and `brokenn := health <= 0`;
- rebuilds `modules` with `Array_Add(GetValidValue(enum_physicalModules, x))` per saved byte, so a
  corrupt byte is clamped to a valid enumerator, never out of range;
- then calls **`loadBrake(); updHealth(); loadLights();`** — and **not** `updUpgrades()`.

`processKeys()` is where the rest of the derivation happens [V]:

```
processKeys() { createContainer(); updTires(); updSpareTire(); updDirt(); updUpgrades(); return true; }
```

`gatherDataFromKey()` returns `gather=false, loadTransform=false` [V] — the ATV is **not** in the
keyed-fixture reconcile lane; it is a normal save-spawned object.

> **`processKeys()` is the single seam a receiver needs**: write the raw fields, call it once, and
> every derived visual, collision, material, speed and constraint state is rebuilt by the game's own
> code — byte-exact, nothing re-implemented on our side.

---

## 6. Identity

- **`key`** (FName) — the ATV's save identity (`getKey` / `getOnlyKey` / `setKey`). A save-placed
  ATV's key is cross-peer stable (both peers load the same save); a runtime-spawned one mints a
  random key per peer via `lib.assignKey → generateRandomKey` until the next save round-trip
  [A 2026-06-15].
- **`containerKey`** (FName) — the ATV's inventory container.

  **`getDefaultContainerName()` = `Conv_StringToName("atv_inventoryContainer" + "|" + key)`** [V].
  So the container key is a **pure function of the ATV key** — deterministic across peers whenever the
  ATV key is, with no eid machinery. `prop_inventoryContainer_atv_C : prop_container_C`, so it is an
  ordinary container and rides whatever container-contents lane already exists.

  **`createContainer()` is a FIVE-rung fallback ladder, and the last two rungs are the hazard** [V]
  (re-measured 2026-08-29 — an earlier revision of this section listed only rungs 1-3 and so omitted
  the theft; `python research/bp_reflection/_fn.py ATV createContainer`):

  | rung | @off | condition | action |
  |---|---|---|---|
  | 1 | `@0` | `IsValid(spawnedContainer)` | `containerKey := spawnedContainer.key` — adopt what we hold |
  | 2 | `@97` | `containerKey == None` | BeginDeferred-spawn a `prop_inventoryContainer_atv_C`, `name := getDefaultContainerName()`, `static := true`, collision **off**; `spawnedContainer.key := getDefaultContainerName()`; `containerKey := spawnedContainer.getKey()` |
  | 3 | `@683` | else | `gamemode.getObjectFromKey(containerKey)` → cast → adopt |
  | 4 | `@840` | rung-3 cast FAILED | `addHint`; **reset** `containerKey := getDefaultContainerName()`; retry `getObjectFromKey` → cast → adopt (`@1177`) |
  | 5 | `@1201` | rung-4 also failed | `addHint`; **`GetActorOfClass(self, prop_inventoryContainer_atv_C)`** — the FIRST such container **anywhere in the world**, with **no key check** → if valid, `@1454` **`spawnedContainer.setKey(getDefaultContainerName())`** — *re-keys the stolen actor to THIS ATV's name*. If invalid, `@1536` `addHint` and `JUMP @153` (spawn fresh). |

  **Rung 5 is identity theft, and it is not hypothetical** [V]: `_map_untitled_211` declares **two**
  `ATV_C` exports (`ATV_2`, `ATV2_2`), so a world with >1 ATV exists in the shipped content. When
  ATV-B reaches rung 5 it takes ATV-A's container actor and renames it — A's `getObjectFromKey`
  then fails, and A walks the same ladder. The re-key is what makes it theft rather than sharing.

  **Design consequence:** `processKeys()` — the "re-derive everything" seam this design wants a
  receiver to call — **begins with `createContainer()`**, so calling it on a mirror can reach rung 5
  and mutate a *different* ATV's container key. `[V]` we do **not** call the ATV's `processKeys` or
  `createContainer` anywhere today (grep: the only `processKeys` hits in our tree are
  `keypad_probe.cpp` / `keypad_sync.h`, a different class), so this is a **design input, not a live
  defect** — the seam owes a guard, or the receiver calls the four `upd*` functions without
  `createContainer`.

---

## 7. Dispatch visibility — what we can and cannot hook

| verb | dispatch | visible to our PE detour? | evidence |
|---|---|:--:|---|
| `playerUsedOn` (the **install** trigger) | `EX_LocalVirtualFunction` from `mainPlayer` | **NO** | [V] raw `$type` in `research/bp_reflection/mainPlayer.json`; same class as the `laptop_C` row at `COOP_DISPATCH_VISIBILITY.md:117` |
| `playerUsedOn_delay` | `EX_LocalVirtualFunction` | **NO** | [V] same dump |
| `upgradeTake` → `takeOffUpgrade` | UI call, then `EX_LocalVirtualFunction` self-call | **NO** | [V] `upgradeTake@18` is `ExecuteUbergraph_ATV(45963)`; ub 46009 is `EX_LocalVirtualFunction takeOffUpgrade` |
| `updUpgrades` / `updHealth` / `updTires` / `updDirt` / `processKeys` | `EX_LocalVirtualFunction` self-calls | **NO** to observe — but all are **BlueprintCallable, so callable BY US** | [V] CFG |
| `insertBattery`, `damageWheel`, `explode`, `padlock_*`, `crowbarOpen`, `driveDetached` | thin thunks into `ExecuteUbergraph_ATV` | **NO** | [V] CFG |
| `intComs_stuffUpgraded` | interface notify | irrelevant — **empty stub** | [V] ub 28564 |

**Consequence.** Every ATV mutation is invisible at the verb. The lane must be **poll the state +
replay through the game's own derive functions**, plus an **act-as-host INTENT** for the discrete,
persistent, shared-world changes (install / remove / put-tire / eject-tire / insert-battery / refuel /
repair). That is the same tier rule the signal-desk lanes converged on (`docs/signals/README.md`:
PE seam > raw-field poll > VM-bracket) and the `order_sync` reference implementation of
`COOP_SYNCER_MODEL` §2b.

---

## 8. The satellite classes

| class | parent | carries | notes |
|---|---|---|---|
| `prop_atvUpgrade_C` | `prop_physModule_C` → `prop_C` | `module` (byte) | 13 leaf subclasses; consumed on install |
| `prop_atvWheel_C` | `prop_C` | `durability`, `dirt`, `fixes`, `cleanVec` | spawned by `ejectWheel` with `fixes-1`, consumed by `putTire` |
| `prop_atvcarbattery_C` | prop family | — | the replacement battery; `insertBattery(player, battery)` → ub 9165 |
| `prop_inventoryContainer_atv_C` | `prop_container_C` | container contents | key = `atv_inventoryContainer\|<atv key>` |
| `prop_radio_atv_C` | prop family | `mediaPlayer` | paused + closed when the radio module is removed |
| `prop_funGun_atv_C` | prop family | — | the guns module's world side [?] |
| `objectViewer_C` | — | `genList()` | the upgrade UI backend (`viewer`) |
| `ui_objectUpgrades_C` | widget | — | `upgradeUI` |
| `event_arirFuelsAtv` (+ `_toolbox`) | event | — | an ariral refuels/repairs the ATV — a **world event that mutates ATV state** |

---

## 9. AS-BUILT coop status (b145)

Source: `src/votv-coop/src/coop/interactables/atv_sync.cpp` (692 LOC),
`src/votv-coop/src/ue_wrap/devices/atv.cpp` (223 LOC),
`include/coop/net/protocol.h` (`AtvStatePayload` **60 B**, `AtvSpawnPayload` 120 B). [V-src]

### 9.1 What is synced
- **Body pose only**: `x,y,z,pitch,yaw,roll` at ~20 Hz on the reliable Normal lane, keyed by the ATV's
  wire key. Occupant-**or**-grabber authority (`IsLocalAuthority`), the host relays a client's stream,
  receivers `PrepareMirror` (physics off, tick off, no rigid-body notify) + a 75 ms `LerpWindow` interp.
- **`occupantSlot`** — the seat reservation, plus a lower-slot-wins tie-break for a simultaneous mount
  (PR #9, arigalit) and a client-side producer deny at `device_occupancy::OnUseInputPre`.
- **`AtvRelease`** — the authority-lost edge: re-enable physics **then** write the inherited linear +
  angular velocity, so a thrown ATV arcs and lands.
- **`AtvSpawn` / `AtvDestroy`** — the synthetic-key lane for an ATV that appears after connect.
- **Connect snapshot** — every indexed ATV with `adopt=1`; `authored` decides whether the joiner
  freezes it or leaves it physics-on and grabbable.

### 9.2 What is NOT synced — the complete gap list
**`modules[]` and all 13 upgrades · `fuel` · `health` · `battery` · `dirt` · `brake` (applied nowhere)
· `lights` · `isDrive` · `brokenn` · `empty` · `trap` · `turbo` · all four tires
(`tires` / `tiresDurability` / `tiresDirt` / `tiresFixes` / `tiresTypes`) · the spare tire ·
`containerKey` + container contents · the seat→puppet attach (a remote driver's body is not placed on
the ATV) · the kerfur passenger · repair · explode · honk · the radio · the map · wheel positions.**

Today a mirrored ATV is a body-shaped shell moving on a stream: no upgrades the local save did not
already have, no damage smoke, no headlights, a frozen fuel gauge, and — see §9.4 — possibly wheels
that are not where the body is.

### 9.3 Correction to the shipped identity lane
`atv_sync.cpp`'s comment says a synth key is minted because *"a bought ATV is delivered ONLY on the
host"*. §0.3 measured that **no shop row sells an ATV**, so that specific premise is false. The
mechanism is not wrong — it fires for **any** ATV first seen after a client connected — but its only
real triggers are non-shop ones (`ufoDropper_car`, an event spawn, a `crafted()` path), all **[?]**.
It also means the `keysHash` divergence gate that the 2026-06-15 doc made Gap B conditional on has
never had a purchased ATV to run against.

### 9.4 The wheels are not in the mirror — a defect candidate [RD]
`PrepareMirror` calls `engine::SetActorSimulatePhysics(actor,false)`, which resolves to
`RootComponent->SetSimulatePhysics(false)` [V-src, `engine_attach.cpp:74-82`] — **the body only**.
`DriveMirrorTransform` then does `SetActorLocation` + `SetActorRotation` — again the root. The four
wheel components remain independently simulating bodies constrained to a root that is teleported ~20
times a second. The game's own `teleportVehicle` re-places the wheels after every actor teleport
precisely because they do not follow. **Whether the mirrored wheels lag, stretch the constraints, or
detach has never been observed** — no smoke scenario drives an ATV, and the ATV lane has never been
hands-on tested. `vehicleGetParts` / `teleportVehicleAdvanced` (§2.6) is the ready-made fix if the
measurement confirms it.

### 9.5 Dead wire bits [V-src]
`ReadPayload` writes `stateBits` bit0 = `isDriven`, bit1 = `brake`, bit2 = `grabbed`. `OnReliable`
reads **only** bit3 (`authored`). Bits 0–2 are produced and never consumed; bits 4–7 are free.

---

## 10. Sync-axis table (the design input)

| axis | native writer | rate | who may author | mirror needs | today |
|---|---|---|---|---|:--:|
| body pose | PhysX on `mesh` | continuous | occupant / grabber | stream + kinematic apply | **synced** |
| wheel poses | PhysX on 4 bodies | continuous | same | `teleportVehicleAdvanced` | **no** |
| `occupantSlot` | `playerSit` / `playerUnsit` | discrete | the mounting peer (self-elected) | seat reservation | **synced** |
| driver body on the seat | `K2_AttachToActor` + hide | discrete | occupant | attach puppet to `playerHit` | **no** |
| `modules[]` | install / `takeOffUpgrade` | discrete, persistent | **arbiter** (intent) | write array + `updUpgrades()` | **no** |
| `fuel` | tick drain / `fuelUp` | continuous + discrete | occupant while driven; the refueller | poke field | **no** |
| `battery` | tick drain (upgrade-parametrised) / `insertBattery` | continuous + discrete | occupant; the inserter | poke + `updBattery()` | **no** |
| `health` | impacts / `toolboxFix` | discrete | the impacted peer; the repairer | poke + `updHealth()` | **no** |
| `brokenn`, `empty`, `isDrive` | `runout()` | discrete | derived from fuel/health/battery | poke, or re-derive | **no** |
| `lights`, `brake`, `turbo` | input | discrete | occupant | poke + `Upd Lights()` / `setBrake()` | **no** |
| `dirt`, `tiresDirt[]` | `diretTire` per tick on ground | continuous | occupant | poke + `updDirt()` | **no** |
| `tires[]`, `tiresDurability/Fixes/Types[]` | `putTire` / `ejectWheel` / `damageWheel` | discrete, persistent | **arbiter** (intent) | poke arrays + `updTires()` | **no** |
| spare-tire trio | spare-box interactions | discrete, persistent | **arbiter** | poke + `updSpareTire()` | **no** |
| `containerKey` + contents | `createContainer` + container use | discrete, persistent | **arbiter** | deterministic key → existing container lane | **no** |
| explode | `health` crossing 0 | one-shot | the authority that crosses | spawn `explosion_C` VFX only | **no** |
| `trap`, `zapped`, `underwater` | world events | discrete | host | poke | **no** |

---

## 11. Open questions (unmeasured — the honest list)

1. ~~**[?] Do mirrored wheels follow the body?**~~ **MEASURED 2026-08-29 [V]** — see §13, the
   instrumented baseline. Short answer: **idle, the two peers agree almost exactly; the moment the
   host authors the ATV they come apart**, and the release path *launches* the other peer's copy.
2. **[?] Where is `hasGuns` consumed?** Not in `ATV.json`. Candidates: `prop_funGun_atv`, `mainPlayer`.
3. **[?] Is `tiresTypes[3]` genuinely never applied?** `setWheelsType` reads indices 0, 1, 2 only.
4. ~~**[?] Is the ATV's key cross-peer stable?**~~ **ANSWERED [V]** — `docs/COOP_SYNC_MAP.md:139`
   records the shipped lane's build+smoke as *"keysHash equal cross-peer"*. So the save-placed ATV's
   key IS stable across peers and the key-index path is sound.
   ~~**[?] Does any ATV ever appear at RUNTIME?**~~ **ANSWERED [V] 2026-08-29 — YES, and the answer
   CANCELS the RULE-2 deletion this question was gating.** The census (below) is over the whole pak,
   not the dumped corpus:

   | step | method | result |
   |---|---|---|
   | 1 | byte-scan the 8.17 GB `VotV-WindowsNoEditor.pak` for the FName `ATV_C `, mapping each hit offset to its mounted index entry (20,873 packages) | **104 owners**: 81 `maps/` + **23 non-map** |
   | 2 | maps are load-time PLACEMENTS, not runtime spawns | excluded |
   | 3 | of the 23, test for the presence of ANY spawn FName (`BeginDeferredActorSpawnFromClass` / `SpawnActor` / `FinishSpawningActor`) in the package bytes | **10 contain none** → cannot spawn anything |
   | 4 | disassemble the remaining 13 (7 already in the corpus + 6 extracted and run through `kismet-analyzer to-json` this pass) | **0 `ATV_C` spawn sites.** The only ATV-adjacent spawns are the ATV spawning its own parts (`prop_atvWheel_C` x3, `prop_atvcarbattery_C`, `prop_inventoryContainer_atv_C`) and `trigger_eventer` spawning `event_arirFuelsAtv_C` / `_toolbox_C` |
   | 5 | **the hole step 4 does not cover: a spawn by ROW NAME rather than by class constant** | `list_props` row **`atv`**: `spawnAsObject = Imports[728] = ATV_C (BlueprintGeneratedClass)`, `hidden = false`, `price = 1`, `canHold = true` |
   | 6 | who consumes it | `lib.PropToObject` @83 — `GetDataTableRowFromName(list_props, prop)` then `IsValidClass(row.spawnAsObject)` → `object := row.spawnAsObject`; `ui_spawnmenu`'s ubergraph reads the same field; both name `spawnPropThroughGamemode` |
   | 7 | reachability bound | the **spawn menu** (cheats-gated). `ui_console` declares no spawn verb (`sv.cheats/check/eject/hash/ping/request/target/upgrades`) |

   **So a runtime ATV is real.** Its own `int_save` key is minted random per peer, so it has no
   cross-peer identity — which is exactly what the v77 synthetic-key machinery
   (`g_savePlacedKeys` / `g_savePlacedActors` / `g_synthForActor` / `AtvSpawn` / `AtvDestroy` /
   `SpawnMirror` / `DestroyMirror` / `isClientSpawnedMirror`) exists to give it. **The lane STAYS.**
   What was actually wrong was only its comment: `atv_sync.cpp:98-101` says *"purchased"* where the
   code's predicate is *"mid-session, not in the baseline set"* — the broader, and correct, thing.
5. **[?] Does `event_arirFuelsAtv` run per-peer?** It mutates ATV state from a world event.
6. **[?] `Fstruct_upgrades`** (`docs/upgrades/SIGNAL_UPGRADES.md`) is the *signal* upgrade store; ATV
   modules live in the ATV's own `getData` bytes. Confirmed disjoint here; whether anything reads both
   is unchecked.
7. **[?] The radio's `mediaPlayer`** — playback state is not in `getData`; whether a mirrored radio can
   be made to play the same thing is unexplored.

---

## 12. Cross-references

- Point-in-time RE/design history:
  `research/findings/vehicles/votv-ATV-quadbike-RE-and-coop-sync-design-2026-06-08.md`,
  `…-Phase1-pose-stream-blueprint-2026-06-08.md`,
  `…-phase2-state-fuel-damage-repair-RCA-2026-06-15.md`,
  `…-grab-airmove-purchased-design-2026-06-15.md`.
- `docs/upgrades/README.md` §5 — three of its `[?]` NEXT items are closed by §3 above.
- `docs/COOP_SYNC_PROFILES.md` §2 — the ATV facet rows.
- `docs/COOP_DISPATCH_VISIBILITY.md` — §7's rows belong there when the lane ships.
- `docs/COOP_WORLD_PROP_DIVERGENCE.md` — fuel / battery / dirt are its exact shape.
- MTA precedent: `Server/…/packets/CVehiclePuresyncPacket.cpp` (pose/rot/vel/turnspeed :122-143,
  damage-gated health :145-171, seat :107-118), `CUnoccupiedVehicleSync.cpp` (single-syncer election
  :59, 99, 144), `CVehicleDamageSyncPacket.*`, `CClientVehicle.{h,cpp}`.

---

## 13. The instrumented baseline (MEASURED 2026-08-29, autonomous two-peer, `[V]`)

Instrument: `coop/dev/atv_probe.cpp` (`[dev] atv_probe=1`), which calls the game's own
`vehicleGetParts()` every 500 ms on every peer and logs the four rig bodies plus the vitals.
Reader: `tools/atv_probe_report.py`. Runs: `python tools/mp.py smoke --duration 90` (idle) and
`--duration 120` with the probe's HOST-only one-shot **sit arm** (`[dev] atv_probe_sit=1`), which
calls `ATV_C::playerSit(localPlayer)` so the ATV is genuinely AUTHORED — an idle ATV is never
mirrored (`atv_sync.cpp:453`), so nothing about a mirror is observable without an occupant.
Both smokes PASS. DLL `436BE41D2A93364A`, b145, proto unchanged.

**The measure is `|wheel - body|`**, which is rotation-invariant, so it isolates suspension travel
from the body tipping or turning.

### 13.1 The rig's own signature

| state | susFR | susFL | susBK |
|---|---|---|---|
| at rest | **93.773** | **93.773** | **71.914** (constant to ~0.001 cm over 80 s) |
| settling after the save-load drop | 92.39 min | 92.39 min | 70.12 min / 72.26 max |
| host, while driven | range **2.33** | — | range **2.32** |

So the suspension is real and its normal working travel is **~2-4 cm**. Any number far outside that
band is not suspension.

### 13.2 Idle: the two peers agree

Over 144 aligned client samples before any authority existed, with both peers running their own
physics on a resting ATV:

| | host | client |
|---|---|---|
| susFR range | 2.72 cm | **2.73 cm** |
| susBK range | 4.29 cm | **4.29 cm** |
| body separation | median **0.3 cm** | |

### 13.3 Authored: they come apart

| | host (driving) | client |
|---|---|---|
| susFR range | 2.33 cm | **18.79 cm** (8x) |
| susBK range | 2.32 cm | **29.58 cm** (13x) |
| susBK excursion | 70.03 .. 72.35 | **58.25 .. 87.83** — 13.7 cm inside and 15.9 cm outside the resting value |
| fuel | 100.000 -> **99.439** | **100.000** (never burned a drop) |
| battery | 100.000 -> **99.909** | **100.000** |
| body separation | | up to **109.9 cm**, 75.2 cm at the last sample |

### 13.4 The mechanism, and one correction to make before reading the table above

**The client's wild numbers are NOT a mirror being deformed by the pose stream** — that was the
first reading and it is wrong. `atv: OnAtvRelease key='ATV' -- physics re-enabled + launch velocity
applied (|lin|=158 cm/s)` fires on the client at 19:54:53, and every client sample outside the rig's
normal band is *after* that line. The client's ATV was **launched at 158 cm/s and rolled away under
its own physics.** The 18-29 cm of "travel" is a loose vehicle bouncing over terrain, not a
constraint rig fighting a teleport.

That makes the release path itself a measured divergence SOURCE, which the C1 design must answer:
`AtvRelease`'s "mirrors un-freeze + inherit" hands the other peer's copy a velocity and lets it go.

### 13.5 What this does NOT establish (stated so it is not over-read)

- The driven window was **19 samples / ~11 s**. It is enough to separate 2.3 cm from 29.6 cm; it is
  not a characterisation of driving.
- **The ordering is unexplained and is an open question**: the host logged `authority released` at
  19:54:53 but its first `driven=1` sample is at **19:54:58**, five seconds LATER, and no sample
  before the release ever read `driven=1`. So what made the host an authority before it was seated
  is not established here. `atv_sync.cpp:168` gates authority on `IsDriven && occupant == local`.
- Whether the client held `preparedAsMirror` during the driven window was **not** instrumented.
- `playerSit` returned with `driven now=0` at the call site; `isDriven` rose ~86 s later. The seat is
  evidently not synchronous, and nothing here measured what fills that gap.
- The ATV's runtime key reads **`ATV`** (uppercase), not the `atv` this document used in §6. The
  container name is `atv_inventoryContainer|<key>`, so the case matters wherever that string is
  rebuilt.
