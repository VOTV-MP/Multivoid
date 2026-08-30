# ATV (quadbike) — full RE + coop sync status   (STATUS: **RE COMPLETE 2026-08-29** · sync **PARTIAL**: rig pose + velocity, arc 1 of the C1 redesign SHIPPED 2026-08-29 `a2a45fc7` — read §14 before §9)

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
`include/coop/net/protocol.h` (`AtvStatePayload` **84 B**, `AtvSpawnPayload` 120 B). [V-src]

> **§9 DESCRIBES b146 (arc 1 commit 1). Everything below the b145 line was REWRITTEN 2026-08-29 —
> read §14 for the as-built and the two design pillars the runs killed.**

### 9.1 What is synced
- **Rig pose + VELOCITY**: `x,y,z,pitch,yaw,roll` plus linear and angular velocity, ~20 Hz on the
  reliable Normal lane while a peer authors it, keyed by the ATV's wire key. Pose authority is
  occupant-**or**-grabber (`IsPoseAuthor`) and the host relays a client's stream. **A receiver does
  NOT freeze it**: the rig runs natively and is CORRECTED — velocity written hard each packet, the
  position error closed by a bounded corrective velocity, and a cut to the authority's pose (the
  game's own `teleportVehicle`) past a speed-scaled threshold OR when the error stops shrinking.
- **An IDLE ATV is synced too**, by the HOST, at 5 Hz gated on change with a 2 s keepalive floor
  (`CUnoccupiedVehicleSync`'s shape). A parked ATV costs one packet every 2 s.
- **`occupantSlot`** — the SEAT: the reservation, the lower-slot-wins tie-break for a simultaneous
  mount (PR #9, arigalit), and the client-side producer deny at `device_occupancy::OnUseInputPre`.
- **`authorSlot`** — WHO streams it (0xFF elects the host as its idle syncer). Separate from the
  seat on purpose: a peer merely GRABBING an ATV must not deny a seat nobody is in. A peer may name
  only ITSELF; only the recorded author may release (client-scoped — slot 0 is exempt).
- **`AtvRelease`** — the authority-lost edge, and NOTHING else: it clears the author. It carries no
  velocity and re-enables no physics, because nothing was ever frozen.
- **The collision guard** — all seven `BndEvt__*ComponentHitSignature` UFunctions are INTERCEPTED,
  and since 2026-08-30 (`8cd0ac25`) only the **two BODY ones** (`mesh`, `car1_Capsule`) are
  CANCELLED on a peer that does not own the ATV's tick, so only one machine authors impulse-damage
  and `explode()`. The five WHEEL delegates run everywhere: cancelling them cost the mirror its rig
  SHAPE (§17), and the residual — a mirror burning its own tire durability and able to
  `ejectWheel` — is narrower than what it replaces. The lane still FAILS CLOSED unless all seven
  RESOLVE, which is now a capability check rather than a policy one.
- **`AtvSpawn` / `AtvDestroy`** — the synthetic-key lane for an ATV that appears after connect.
- **Connect snapshot** — every indexed ATV with `adopt=1`, carrying pose AND velocity AND
  `authorSlot`, so an ATV airborne at the join arrives moving and lands.

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
mechanism is not wrong — it fires for **any** ATV first seen after a client connected.

**TRIGGER MEASURED 2026-08-29 (§11.4), and it is none of the ones this section speculated.** The
whole-pak census found **zero** blueprints that spawn `ATV_C` by class constant — so `ufoDropper_car`,
an event spawn and a `crafted()` path are all ruled OUT, not merely `[?]`. The one real trigger is
`list_props` row `atv` (`spawnAsObject = ATV_C`, `hidden = false`) reached via `lib.PropToObject` →
`spawnPropThroughGamemode` from `ui_spawnmenu`. So the lane STAYS and its comment was the only wrong
part (fixed in `d737321c`). The `keysHash` divergence gate the 2026-06-15 doc made Gap B conditional
on has still never been run against a runtime-spawned ATV.

### 9.4 The wheels are not in the mirror — a defect candidate [RD]
`PrepareMirror` calls `engine::SetActorSimulatePhysics(actor,false)`, which resolves to
`RootComponent->SetSimulatePhysics(false)` [V-src, `engine_attach.cpp:74-82`] — **the body only**.
`DriveMirrorTransform` then does `SetActorLocation` + `SetActorRotation` — again the root. The four
wheel components remain independently simulating bodies constrained to a root that is teleported ~20
times a second. The game's own `teleportVehicle` re-places the wheels after every actor teleport
precisely because they do not follow. `vehicleGetParts` / `teleportVehicleAdvanced` (§2.6) is the
ready-made fix if the measurement confirms it.

**SUPERSEDED 2026-08-29 by arc 1 (§14): `PrepareMirror` and `preparedAsMirror` NO LONGER EXIST, so
this section's defect cannot occur — a mirror is never kinematic and its wheels are the game's own.
The paragraph below is kept as the point-in-time record of the b145 lane; do not send anyone to
`preparedAsMirror`, it is a dead symbol. What is STILL open is narrower and stated in §14.5: no ATV
has ever been DRIVEN in any run, so the corrector under load is unexercised.**

**STATUS UPDATED 2026-08-29 — half of this section's "never observed" is now false, and the other
half is still true.** A smoke scenario now DOES drive an ATV (the probe's sit arm, §13), and the rig
has been instrumented on both peers. What §13 measured is that the client's rig went far outside its
normal band — but the cause was `AtvRelease` **launching** the client's copy at 158 cm/s, not a
mirror being deformed by the stream. **Whether the wheels of an actively MIRRORED ATV lag or stretch
is still `[?]`**: the run never confirmed the client held `preparedAsMirror` during the driven
window, so the specific claim in this section has not been tested. What it needs is one more arm —
assert `preparedAsMirror` on the receiver and sample across it. The ATV lane remains never
hands-on tested.

### 9.5 Dead wire bits [V-src]
`ReadPayload` writes `stateBits` bit0 = `isDriven`, bit1 = `brake`, bit2 = `grabbed`. `OnReliable`
reads **only** bit3 (`authored`). Bits 0–2 are produced and never consumed; bits 4–7 are free.

---

## 10. Sync-axis table (the design input)

| axis | native writer | rate | who may author | mirror needs | today |
|---|---|---|---|---|:--:|
| body pose | PhysX on `mesh` | continuous | occupant / grabber | stream + **correct a simulating rig** | **synced** |
| wheel poses | PhysX on 4 bodies | continuous | same | ~~`teleportVehicleAdvanced`~~ **nothing — the rig is never parked, so the wheels are the game's own** | **synced by construction** |
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

1. ~~**[?] Do mirrored wheels follow the body?**~~ **ANSWERED 2026-08-30 [V] — YES.** The b145
   baseline (§13) could only say what an IDLE pair did, because no run had ever driven one. The first
   driven run (§15) measured a mirror's suspension over a 20 s driven window at
   **1.56 / 2.08 / 5.67 cm** against the author's **2.08 / 2.29 / 2.63 cm** — ratios 0.75 / 0.91 /
   2.16, i.e. the same regime, and an order of magnitude above the 0.001 cm a rigid rig holds. The
   mirror is not a corpse. What the same run DID find is a different question the doc had not asked:
   it TRAILS (§15.2).
   *(Items 2, 3, 5, 6, 7 re-checked against the tree at `18edd22a` on 2026-08-30: `hasGuns`,
   `tiresTypes`, `event_arirFuelsAtv` and `mediaPlayer` each appear in **zero** files under
   `src/votv-coop/src/`, so nothing has shipped for any of them. They are STILL OPEN, not
   stale-open.)*
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
   | 1 | byte-scan the 8.17 GB `VotV-WindowsNoEditor.pak` for the FName `ATV_C` + a NUL terminator (written in words: a literal NUL byte here made git treat this whole doc as BINARY and every diff of it unreadable), mapping each hit offset to its mounted index entry (20,873 packages) | **104 owners**: 81 `maps/` + **23 non-map** |
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
   What was actually wrong was only its comment: `atv_sync.cpp:123` says *"purchased"* where the
   code's predicate is *"mid-session, not in the baseline set"* — the broader, and correct, thing.
4b. ~~**[?] Can we write the whole rig's velocity, or only its root?**~~ **ANSWERED 2026-08-30 [V] —
   ONLY THE ROOT, by this route.** A live census of the ATV's component PROPERTIES
   (`[ATVP] rig component`, `coop/dev/atv_probe.cpp`) found `mesh` at `off=0x570`
   (`StaticMeshComponent`) and **all seven** of `car1_Capsule`, `car1_frontWheel_R`,
   `car1_frontWheel_L`, `car1_frontWheelRoot`, `car1_backWheel_R`, `car1_backWheel_L`,
   `car1_backWheelRoot` reported **NOT A PROPERTY on this class**. They are SCS components,
   reachable only through the actor's component array — and `SetAllPhysicsLinearVelocity` would not
   reach them either, since it addresses the bodies WITHIN one component and these are separate
   components. So the "write all five bodies" fix is not buildable as designed. See §16.4.
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
mirrored (`atv_sync.cpp:717`), so nothing about a mirror is observable without an occupant.
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
  is not established here. `atv_sync.cpp:188` gates authority on `IsDriven && occupant == local`.
- Whether the client held `preparedAsMirror` during the driven window was **not** instrumented.
- `playerSit` returned with `driven now=0` at the call site; `isDriven` rose ~86 s later. The seat is
  evidently not synchronous, and nothing here measured what fills that gap.
- The ATV's runtime key reads **`ATV`** (uppercase), not the `atv` this document used in §6. The
  container name is `atv_inventoryContainer|<key>`, so the case matters wherever that string is
  rebuilt.

---

## 14. Arc 1 commit 1 — AS-BUILT (2026-08-29, `070c7d29` + `a2a45fc7`, proto 146)

> **READ §15 FIRST.** On 2026-08-30 an ATV was DRIVEN cross-peer for the first time, and that run
> answered §11.1, proved §14.5's collision-guard cancel path, and **falsified §14.6's attribution
> in this doc's own words**. §14.5 and §14.6 below carry supersede stamps; where they and §15
> disagree, §15 is what was measured.

**Status: AS-BUILT, autonomous evidence only — NOT hands-on.** DLL `405E4F67CB5FEADC`, deployed to
all four folders, two-peer smoke PASS. Design of record:
`research/findings/vehicles/votv-ATV-arc1-mirror-model-IMPL-2026-08-29.md` (local-only).

### 14.1 What the lane is now
A peer that does not author an ATV **runs the rig natively and is corrected toward the authority**.
The freeze/teleport model is deleted whole (RULE 2), and with it `PrepareMirror`, `ReleaseMirror`,
`SetBrainEnabled`, `DriveMirrorTransform`, the `LerpWindow` interp, the `authored` wire bit and
`AtvRelease`'s six velocity floats. What distinguishes a mirror is exactly one thing: it may not
author COLLISION damage.

| pillar | as-built |
|---|---|
| P1 correct, don't teleport | velocity written hard per packet; position error closed by a bounded corrective velocity sized over the MEASURED packet interval; cut to the authority's pose past a speed-scaled distance, past 45 deg on any axis, or when the error stops shrinking for 5 packets |
| P2 brains off | **RETIRED — see §14.3.** The tick stays ON everywhere |
| P3 collision guard | 7 `BndEvt__*ComponentHitSignature` interceptors, cancel-on-true when the peer does not own the tick; FAILS CLOSED (lane inert without all seven) |
| P4 single syncer | `ownsTick` = pose author, else the host. `[V]` host `owns=1` / client `owns=0` in a real log; A4 (one owner per ATV per second) PASS |

### 14.2 The release path DISSOLVED rather than being fixed
The owed question was *"P1/P4 must answer the release path"*. Under this model there is no release:
nothing froze, every packet already carried the velocity, and the stream does not stop — `authorSlot
== 0xFF` hands the ATV to the host's idle syncer. `AtvRelease` now clears the author and does nothing
else. That deletion IS the fix for §13.4's measured 158 cm/s launch.

### 14.3 `[V]` THE TICK IS NOT THE BRAIN — a pillar the run killed
"Brains OFF, physics ON" was P2 of the converged design. The first two-peer run refuted it: from a
byte-identical start the tick-off mirror ended **42.7 cm** away. The bytecode says why —
`ExecuteUbergraph_ATV @29894` calls `mesh.SetCenterOfMass(VLerp(..., tirescount/4))`
**unconditionally, every frame**, before any gate. Centre of mass is rig CONFIGURATION re-applied per
tick, not gameplay logic, so a rig whose tick is off rests somewhere else. And everything tick-off was
meant to stop is ALREADY single-peer by the game's own gating: `@29949 IFNOT(isDriven) POP` guards
`applyWheelTorque`, and every battery-drain term at `@33970-@34123` is
`SelectFloat(x, 0, isDriven|isDrive|lights|turbo)` — all local-only. Measured effect of restoring the
tick: horizontal agreement **13.2 cm → 0.3 cm**.

### 14.4 `[V]` A NUDGE CANNOT MOVE A BODY AT REST — the second reversal
Velocity-based correction is right for a moving body and powerless against a resting one (a 20 cm/s
corrective velocity is erased by gravity in 20 ms). The corrector therefore watches itself: if the
error stays outside the deadband and refuses to shrink for 5 consecutive packets, it CUTS. It counts
packets, not seconds, so it is cadence-independent, and it needs no velocity threshold — velocity was
the quantity lying about whether convergence was possible.

### 14.5 What the runs did NOT establish, stated so it is not over-read
- **The corrector under LOAD has never run.** The probe's sit arm calls `ATV_C::playerSit(localPlayer)`
  and the log reads `SIT fired ... (driven now=0)` — the player is never actually seated, so `driven=1`
  appears in ZERO samples across four runs and the acceptance's A1 arm is INCONCLUSIVE **by its own
  design**. Fixing the arm is the next instrument job. This also means §9.4's mirrored-wheel question,
  though it can no longer occur *by construction*, has still never been watched under load.
- ~~**The collision guard armed 7/7 on both peers but its CANCEL path never fired**~~ **CLOSED 2026-08-30 [V] — it fired: 19,399 cancelled / 3,911 allowed on the client and 2,587 / 22,409 on the host, and the ratio is the design (see §15.4).** Original text: no ATV collided in
  any run. Armed is not fired (`docs/COOP_DISPATCH_VISIBILITY.md`'s coin-lane row is the precedent).
- **NOT hands-on.** Everything here is autonomous.

### 14.6 ~~`[V]` A residual that is NOT this lane's defect — the peers' WORLDS differ under the ATV~~

> **STATUS 2026-08-30 (SECOND REVISION — read §16, not this box and not §15.3).** This section was
> superseded by §15.3, and §15.3 has since been RETRACTED, so the chain below no longer resolves:
> a supersede stamp pointing at a withdrawn finding leaves nothing standing. What §16 measured is
> that the conclusion here (*"the host has support under it that the client does not"*) was
> **untestable at the time it was written**, because both cut paths write a velocity onto the rig
> IMMEDIATELY after teleporting it (as of `18edd22a`, `atv_corrector.cpp:214-216` and `:273-275`,
> both now routed through `WriteMirrorVelocity`; the lines first cited here, `:125-126`/`:144-145`,
> moved the same day the citation was written) — so the "nine cuts
> that fell back" were nine teleport-**and-push** events and not one teleport-and-let-rest. The
> experiment that separates ground from lane had never been run. §16 runs it.
>
> **SUPERSEDED 2026-08-30 — THE ATTRIBUTION BELOW IS WRONG, and it is kept because being wrong in
> this particular way is the lesson.** The reasoning was: the gap is constant, it survives a rig
> teleport, therefore it is the ground. Every one of those observations was true. What was never
> tested is the one thing that would have separated "the ground here" from "something the lane
> acquires": **move the ATV and look again.** The first driven run did that. Starting at the same
> parking spot the two copies were **3.5 cm apart in Z**; after a 20 s drive that ended ~4 km away
> they were **39.6 cm apart** — the gap is ACQUIRED DURING THE DRIVE and then persists, so it is
> not a property of the parking spot and not the terrain. See §15.3. Original text follows.
Every run ends with the two copies **40.5 cm apart in Z only, exactly constant**. It survives a full
rig teleport onto the host's pose: the corrector's cut fired **nine times** in one run and the client's
copy fell back to the same 40.5 cm each time. From an identical save pose the host's ATV settles UP
3.5 cm and the client's falls 37 cm — so the host has support under it that the client does not. No
pose lane can hold a mirror where its own world has no floor. `tools/atv_probe_report.py` now
ATTRIBUTES this instead of blaming the corrector. **File against the world / save-transfer lane, not
against C1.**

---

## 15. `[V]` The first DRIVEN cross-peer measurement (2026-08-30, autonomous, NOT hands-on)

Everything in §13 and §14 was measured on an ATV that **nobody ever drove**. That was not a choice:
the probe's arm called `ATV_C::playerSit`, which is a **dead stub** on this build — it writes
ubergraph variable `K2Node_Event_player_18`, which has zero readers anywhere in
`ExecuteUbergraph_ATV`, and jumps to `ExecuteUbergraph_ATV(9122)`, a bare `EX_PopExecutionFlow`
`[V, disasm]`. Four runs called it, logged "SIT fired", and seated nobody.

**The live seat verb is `actionName(player, hit, name)` with `name == "sit"` → uber `@46046`**, gated
three deep before the seat body at `@5616` `[V, disasm]`:

| gate | test | else |
|---|---|---|
| `@46420` | `abs(player.fallVeloc.Z) < 800` | punched off (`@46870`) |
| `@46522` | `player.checkEquip()` reports EMPTY hands | `addHint` (`@46753`) |
| `@46645` | `playerHit` overlaps nothing at index 0 | `addHint` (`@46659`) |

The seat body attaches and teleports the player onto `playerHit`, possesses the ATV and sets
`isDriven := true` (`@6227`) — **it needs no proximity of its own**, so an instrument may call it from
wherever the player happens to be. Gate 2 is why the arm no longer runs on the host: the host's test
save has the player holding a `prop_coingun_C` (`checkEquip.empty=0`, measured), so the arm runs on
whichever peer sets `[dev] atv_probe_sit=1` and the fresh-booted CLIENT drives — which also exercises
the harder direction, a client-authored ATV mirrored by the host.

Torque needs `isDriven` (`@29949` gates `applyWheelTorque`) **and** a non-zero `torqAlpha`, whose
producer bails whole at `@34866` on `empty || brake || brokenn || underwater || battery <= 0`. A
parked ATV is on its handbrake, so the arm releases it through the game's own `setBrake()`.

### 15.1 The run
`research/atv_runs/20260830-002246/` (archived — mp.py deletes each peer's log at launch). DLL
`B1E659B76A0C01A2`, proto 146, two-peer LAN smoke PASS, **20.2 s of continuous driven time, zero
ejections**. The throttle is PULSED (250 ms on / 750 ms off): at full throttle the rig covered 9.6 m
in 2.5 s, hit something, and the game **ragdolled the driver out at 600 cm/s** — a base is not a test
track, so the arm banks cumulative driven time and re-seats after a crash.

### 15.2 `[V]` The mirror TRAILS, the trail scales with SPEED, and the warp never fires
**Two runs, and the second one corrected the first — read both before quoting a number.**

| run | DLL | driven path | peak speed | trail mean | trail max | `warpD` at that speed | A5 |
|---|---|---|---|---|---|---|---|
| `20260830-002246` | `B1E659B76A0C01A2` | 78 m | ~1300 cm/s | 134 cm | **438 cm** | ~850 cm | FAIL |
| `20260830-003415` | `7E4D7A1D8D75DD03` | 33 m | ~780 cm/s | 20 cm | **70 cm** | ~590 cm | PASS |

The first run's 438 cm was published as "the mirror trails by up to 4.4 m" **as if it were a property
of the lane. It is not — it is a property of driving at 13 m/s**, and the second run says so: same
build family, same arm, same 20 s window, a sixth of the trail because the ATV happened to be pointed
somewhere that let it go a third as far. The arm steers nothing, so route and speed are not controlled
between runs and **no single run may state a trail figure as a lane property.**

What BOTH runs agree on: `atv_corrector.cpp:32-33` warps past
`kWarpBaseCm + kWarpPerSpeedS * |v|` = `200 + 0.5*|v|` cm. At 1300 cm/s that is ~850 and the trail
reached 438 (52% of it); at 780 cm/s it is ~590 and the trail reached 70 (12%). **The warp arm did not
fire in either run.**

> **CORRECTION (same session): the MTA comparison first written here was WRONG, and it was wrong in
> the way that is hardest to notice — across UNIT SYSTEMS.** I wrote that MTA's
> `CClientVehicle::UpdateTargetPosition:3867` threshold `15 + 10*|v|` is "small base, large speed
> term, the opposite shape" to ours. `[V]` from the vendored source: the full expression is
> `(VEHICLE_INTERPOLATION_WARP_THRESHOLD + VEHICLE_INTERPOLATION_WARP_THRESHOLD_FOR_SPEED *
> vecVelocity.Length()) * GetGameSpeed() * TICK_RATE / 100` with `15` / `10`
> (`CClientVehicle.cpp:77-78`) and `TICK_RATE = iPureSync = 100` by default
> (`CTickRateSettings.h:16`), so the trailing factor is ≈1 — but it is compared against a distance in
> **GTA world units**, and ours is in **centimetres**. A 15-unit base is 15 m ≈ 1500 cm if a GTA unit
> is a metre, i.e. **7.5x LOOSER than our 200 cm base, not tighter.** And the speed term cannot be
> compared at all: MTA's velocity units are not established anywhere in the vendored tree, so
> `10 * |v|` and `0.5 * |v|` are not commensurable. **Both halves of the original claim are
> withdrawn.** What remains is only about our own lane.
>
> **And the units fact was already in this repo, three lines above the constant I quoted.**
> `atv_corrector.cpp:28-29`: *"Warp is speed-scaled after CClientVehicle.cpp:3901 (their 15 + 10\*|v| is
> in GTA units); ours is sized off the measured rig"*. Whoever ported the number did the conversion
> and wrote it down. I opened MTA's file and read MTA's line, and never read our own four lines
> wrapped around the value I was comparing it against.

### 15.2a `[V]` What the trail actually does: `trail ≈ 0.0063 * speed^1.52`
Pooling every driven second from both runs where exactly one peer owned the tick and the author was
moving faster than 20 cm/s (**n = 19**, log-log fit, **R² = 0.73**):

| author speed (cm/s) | measured trail (median) | fit | our warp threshold |
|---|---|---|---|
| 100 | 6 cm | 7 | 250 |
| 200 | 21 cm | 20 | 300 |
| 400 | 44 cm | 57 | 400 |
| 800 | 235 cm | 163 | 600 |
| 1200 | 351 cm | 302 | 800 |
| 1600 | 284 cm | 468 | 1000 |

**The trail grows super-linearly (~v^1.5) while the threshold grows linearly**, so headroom narrows
with speed — 40x at 100 cm/s, ~3.5x at 1600 — but **it never crossed in the measured range.** n=19 over
two uncontrolled routes is a weak fit and the 1600 row already sits below the line; treat the exponent
as a shape, not a coefficient.

> **RETRACTED 2026-08-30 (§16): the recommendation below — "arc-1 commit 2 should look at
> `kCorrGain` and the packet cadence first" — is WITHDRAWN.** The corrector's convergence rate is
> not the defect. The cut lands and the rig returns to the same Z within 500 ms; the [ATVC]
> instrument shows the author reporting `|v| = 0.0` while the mirror free-falls. A gain has
> nothing to act on. The half of this section that stands is the measured trail fit itself.

**This inverts the recommendation the first version of this section implied.** The warp is a
last-resort net and our runs never needed it; a net that does not fire is not evidence that the net is
wrong. What produces a 4.4 m trail at 13 m/s is the CORRECTOR's convergence rate, not the warp
threshold — so **arc-1 commit 2 should look at `kCorrGain` and the packet cadence first, and leave
`kWarpBaseCm` / `kWarpPerSpeedS` alone until something shows the net failing.**

Graded from now on by acceptance arm **A5** (`TRAIL_MAX_CM = 150`, a stated design ceiling of about
one vehicle length). **A5's fixed-cm shape is known-weak**: it passed the slow run and failed the fast
one, so it is partly measuring the route. Normalising it by the warp threshold was tried and
**rejected by measurement** — it only cuts the between-run spread from ~6x to ~4x, because the trail
grows as v^1.5 and the threshold linearly, so the ratio is still route-dependent. Until a run can hold
a route, A5 is a tripwire rather than a metric: a FAIL is worth reading, a PASS proves less than it
looks.

*(Why the DLLs differ: run 1 ran on bytes another session deployed to the shared rig between my deploy
and my launch — caught only because the run archive records the deployed sha256. Run 2 is a rebuild
from the same tree. The two runs are not a DLL A/B; the measured difference tracks speed, not bytes.)*

### 15.3 ~~`[V]` The Z residual is ACQUIRED, not inherent — §14.6 corrected~~

> **RETRACTED 2026-08-30 (§16). The gap is not acquired by DRIVING; it is the resting state of a
> MIRRORED ATV, and driving one temporarily CLOSES it.** Time-aligned across four runs the pair's
> Z gap goes 3.5 cm parked → ~5 cm while driven → 25-40 cm parked again, and the change happens in
> the single sample where authority moves. The numbers in the table below are real and the
> before/after pair is real; the word "acquired" and the causal story attached to it are not. The
> "39.6 cm at 4 km" row is additionally the run whose ATV ended UNDERWATER (§15.2's run 1).
| phase | Z gap (host − client) | horizontal |
|---|---|---|
| idle, before the drive | **3.5 cm** | 3.4 cm mean |
| driven | mean −2.7, min −61.8, max +115.2 | mean 129.5, max 437.9 |
| idle, after the drive (~4 km away) | **39.6 cm, constant** | 3.4 cm mean |

The two copies agree at the parking spot to 3.5 cm and are 39.6 cm apart in Z after the drive. So the
40 cm is not the terrain under the parking spot; the lane acquires it while driving and the corrector
never closes it — the stall detector cuts to the authority's pose and the rig settles back.

**Run 2 (`20260830-003415`) reproduces the SHAPE on a different route: settled gap 25.4 cm, again
dominated by Z.** So "acquired during the drive, then persists" holds across both runs even though the
magnitude does not — which is the right level to state it at, and is exactly what §15.2's trail figure
failed to do. **Open, and now correctly scoped to this lane rather than filed against
world/save-transfer.**

### 15.4 `[V]` The collision guard's cancel path, proven
Counters over the run: **client 19,399 cancelled / 3,911 allowed; host 2,587 / 22,409.** The ratio is
the design, not an anomaly: the client authored the ATV for ~20 s of a ~180 s run, so it cancels for
most of it, and the host — the idle syncer whenever `authorSlot == 0xFF` — allows for most of it and
cancels only during the client's authorship. §14.5's "armed but never fired" is closed.

### 15.6 `[?]` A4's double-owner second at the authority handoff
Run 2 failed A4 with **one second (1920, the claim edge) in which both peers reported owning the
tick**. Run 1 passed it. That is the shape of an assertion race rather than a bug in either peer's
predicate: `OwnsTickFor` elects the host whenever `authorSlot == 0xFF`, so between a client seating
itself and the host receiving `AtvState` with the new `authorSlot` there is a round trip in which both
sides answer yes. `COOP_SYNCER_MODEL.md` §2b's rule — authority is ASSIGNED, never asserted — says the
claim should be an intent the host grants, not a fact the client publishes. **Open; sized as arc-1
commit 2, and A4 already grades it.**

### 15.7 `[V]` A2 is REPRODUCIBLE, and it is the lane's one standing failure
Three driven runs now, three different routes, three different builds:

| run | settled gap | dominated by |
|---|---|---|
| `20260830-002246` | 54.2 cm | Z |
| `20260830-003415` | 25.4 cm | Z |
| post-extraction (`C67CEC72AD2E31C3`) | 30.5 cm | Z |

A1, A3, A4 and A5 pass in the post-extraction run (A5 at 48 cm — a slow route again, see
§15.2). **A2 has failed in every driven run**, always Z-dominated, magnitude varying with the route.
Combined with §15.3's before/after pair this is the lane's one reproducible defect, and it is the
thing arc-1 commit 2 should aim at along with the convergence rate.

### 15.5 What this run still does NOT establish
- **NOT hands-on.** Autonomous throughout, all three runs.
- **(RETRACTED 2026-08-30, §16 — the convergence rate is not the defect and this bullet sent the
  next session at the wrong knob.)** ~~The corrector's own convergence is UNTUNED.~~ §15.2a says the trail is the corrector's rate rather
  than the warp net, and nothing has changed `kCorrGain` or the packet cadence -- that is arc-1
  commit 2, and it now has a home to change: `coop/interactables/atv_corrector.cpp` (extracted
  2026-08-30 for exactly this, `f802104e`).
- **A2 FAILS in all four driven runs** (40.2 / 25.4 / 30.5 / 38.9 cm settled -- run 1's figure
  is 40.2, not the 54.2 first published: A2 was comparing each peer's own last sample and the
  client's log ended 70 s before the host's) -- §15.7. A1 and A3 pass in
  all three; A4 passed runs 1 and 3 and failed run 2 (§15.6); A5 failed run 1 and passed runs 2 and 3
  (§15.2), which is the route, not a fix.
- The client-side mirror is unmeasured in the driven window (1 sample): the ATV is authored BY the
  client, so the host is the only mirror there is. Grading the client's mirror needs a host-driven
  run, which needs a host save whose player has empty hands.
- Nothing here measures a THIRD peer, and A4's single-syncer arm has only ever seen two.


---

## 16. ~~`[V]` A MIRRORED PARKED ATV FREE-FALLS — the root~~ — SUPERSEDED BY §17 (same day)

> **READ §17 FIRST. The ROOT stated below is wrong; the MEASUREMENTS in it are not.** §16 held
> that a mirrored ATV sinks because assigning a velocity to a settled rig wakes it. A four-cell
> single-variable experiment the same evening acquitted the pose corrector entirely: with the
> COLLISION GUARD disabled the corrector produces the *best* result of the four cells (A2 = 3.0 cm).
> What survives from §16: the `[ATVC]` instrument and its wire-velocity readings, §16.4's component
> census, §16.6's hook boundary, and the at-rest write rule (which is still correct, just not the
> cure it was believed to be). What does NOT survive: "the write is the only thing left that can be
> causing the fall" — the guard was the other thing, and it was never in the frame.

## 16-orig. `[V]` A MIRRORED PARKED ATV FREE-FALLS — the root as understood on 2026-08-30 morning

Four driven runs, four A2 failures. This section replaces the attribution in §14.6 and §15.3, both
of which are now retracted, and withdraws §15.2a's and §15.5's "tune `kCorrGain`" recommendation.

### 16.1 What the instrument had never recorded
Nothing sampled the value this lane WRITES. The probe logs each peer's own root velocity every
500 ms; the corrector acts on the RECEIVED `AtvStatePayload` velocity at packet arrival — a
different quantity at a different instant. `coop/interactables/atv_corrector.cpp` now logs `[ATVC]`
on every cut and once a second otherwise, and the first run with it says:

```
[ATVC] NUDGE dist=10.0 cur.z=6272.5 wire.z=6282.5 wireLin=(-0.0,0.0,-0.0) |v|=0.0 stall=1
[ATVC] NUDGE dist=45.9 cur.z=6236.6 wire.z=6282.5 wireLin=(-0.0,-0.0,-0.0) |v|=0.0 stall=2
```

**The author holds one Z to the decimal and reports zero velocity. The mirror falls 46 cm in about
a second — free fall.** It comes to rest 25-40 cm low and stays there, dead flat, for a hundred
samples. Cutting it back lands (run 1 client, 00:21:06: body Z 5405.2 → 5430.5 the sample after)
and it falls again within 500 ms, every time.

### 16.2 It is not the ground, and the evidence is the authority flip
The same peer's own rig, at the same XY (4.3 cm of horizontal movement across the handoff), rested
at Z 6176.7 while it authored and at 6153.4 twenty seconds later while it mirrored. Time-aligned,
the pair's Z gap across a run runs **3.5 cm parked → ~5 cm while driven → 25-40 cm parked again**,
and the whole change lands in the single sample where authority moves. Both peers lose the occupant
at that same instant and move in OPPOSITE directions, so occupancy cannot explain the sign; the
only variable that tracks it is which peer is running the corrector.

### 16.3 Two mechanisms were proposed and both were wrong
Stated so neither is re-derived. **(a) A per-packet ratchet** — the constraint solver fighting a
root-only velocity assignment, position ratcheting down between packets. Killed by the data: a
ratchet oscillates at the sample rate and this rig is dead flat after the fall. **(b) A downward
velocity handed over the wire** — the author's own copy settling after its cut, its sampled
velocity biased downward, written onto the mirror. Killed by the [ATVC] lines above: `|v| = 0.0`.

What is left is the write itself. `SetActorRootPhysicsVelocity` resolves to
`UPrimitiveComponent::SetPhysicsLinearVelocity` on the root component with `bAddToCurrent=false`
(`engine_attach.cpp:182-196`), and assigning a velocity WAKES a body. We were waking a settled rig
every packet. **The precise PhysX consequence is still `[?]`** — what is `[V]` is that the peer
being written to falls and the peer not being written to does not.

### 16.4 `[V]` A rig-wide velocity write is NOT reachable by property
The census (`[ATVP] rig component`) resolved the ATV's component properties on a live instance:

| property | result |
|---|---|
| `mesh` | `off=0x570`, non-null, `StaticMeshComponent` |
| `car1_Capsule`, `car1_frontWheel_R`, `car1_frontWheel_L`, `car1_frontWheelRoot`, `car1_backWheel_R`, `car1_backWheel_L`, `car1_backWheelRoot` | **NOT A PROPERTY on this class** — all seven |

So the wheels are SCS components reachable only through the actor's component array, not by name
off the class, and `SetAllPhysicsLinearVelocity` would not reach them either (it addresses the
bodies WITHIN one component; these are separate components). The "write all five bodies" fix is
not buildable the way it was designed. The invariant it came from still stands and is §16.6.

### 16.5 The fix (BUILT + RUN 2026-08-30, `FBE271E87BABE8F0`, autonomous, NOT hands-on)
`atv_corrector.cpp`: when the AUTHOR's reported linear and angular velocity are below
`kRestLinCmS`/`kRestAngDegS`, the mirror is **not written to at all** — no wire velocity, no
corrective term. Out of band, `TeleportRig` once and leave it; bounded at `kRestMaxReplaces = 3`,
after which it says so rather than teleporting forever (a corrector owes a convergence check on
every arm it has, and this is the at-rest arm's).

MTA's shape (`CUnoccupiedVehicleSync.cpp:194/311`, server `:315-321`): `bSyncVelocity` is set only
when the velocity is non-negligible and the receiver writes velocity only under that flag — MTA
never writes velocity onto a resting mirrored vehicle either. Divergence, cited in source: they
spend a wire bit, we test the received value. **Their constant is NOT ported** — MTA's `0.1` is in
units the vendored tree establishes nowhere. Ours: parked author 0.0, coasting 27 / 8.2 / 2.6 / 1.0
/ 0.2 cm/s, driven 780-1500.

**This branch is also the experiment §14.6 needed.** Both existing cut paths write a velocity
immediately after `TeleportRig`, so in four runs the rig was never once put down and left to rest.
If it still will not hold, the difference really is under the vehicle — and the bounded arm says
that in one line instead of asserting it.

### 16.6 `[?]` The residual, and the rig's boundary is NOT fixed
The class is *every write this lane makes to a mirrored rig addresses one of its bodies*. §16.5
removes the instance where that measures 25-40 cm; the moving-mirror instance (4.8 cm, one sample)
survives it and nothing grades it — A5 grades horizontal trail only.

And the rig is not five bodies. **A player can tie arbitrary physics props to the ATV with the
hook** (`prop_hook_C` fires a `hook_C` carrying its own A↔B `PhysicsConstraint`; its flight trace
uses the `statDynPhysVeh` object set, so a vehicle is a target by design, and `attach_a` rejects
only Characters, child actors and other hooks). **The hook lane has NO implementation in this tree
— zero symbols, every row of `docs/items/hook.md` §2 is a GAP** — so one peer's ATV can be coupled
to a crate the other peer does not know exists, and:

- a mirrored ATV can be held off the authority's pose by a constraint the author cannot see, which
  is why §16.5's give-up line names the CLASS ("something local to this peer") instead of blaming
  the terrain;
- `TeleportRig` on a hooked ATV yanks whatever is tied to it, on that peer only;
- the seven ComponentHit interceptors guard the ATV's own components and say nothing about a
  coupled prop's.

None of this is measured against a real hooked ATV. It is recorded because the ATV design has been
reasoning about a closed five-body rig and the game does not guarantee one.

### 16.7 `[V]` THE RUN: the fix removed the write, and the residual SURVIVED it
Deployed `FBE271E87BABE8F0`, 150 s two-peer smoke, driven arm fired (peak torqAlpha 1035.8).

**The at-rest branch fires, and it is the experiment §14.6 needed.** Three times in one run the
mirror was placed on the author's pose and LEFT THERE — no velocity write after the teleport, the
first time that has ever happened in this lane — and three times it would not hold:

```
atv: a parked mirror would not stay on the authority's pose after 3 re-places
     (last error 40.5 cm) -- something local to THIS peer is holding the rig off it
```

So the residual is **not** our velocity write. Waking the rig every packet was real and is gone;
what is left is something on the receiving peer that holds the rig 25-40 cm off the author's pose
even when nothing is pushing it. **That makes §14.6's original reading the supported one** — earned
by the experiment this time rather than assumed from a cut that was always followed by a push.

**A2 still FAILS: 36.6 cm.** The pair is 3.5 cm apart parked before the drive (identical to every
prior run) and 23.3 cm apart at the release. Unchanged in shape.

**A6 passed both handoffs this run (−13.2 / −17.6) and that is NOT evidence the fix worked.** Its
"before" sample is taken at the release instant, when the rig may still be coasting — here the pair
was already 45.6 cm apart at that moment for drive-related reasons, so the arm measured a gap
CLOSING. The `still()` guard covers the AFTER sample only. **Known weakness, not fixed:** A6's
baseline needs to be the last instant both copies were at rest BEFORE the claim, not the release
edge. Until then a green A6 means less than a red one.

**A1 back FAILED for the first time** (mirror 6.13 cm vs author 2.10, x2.92 over the 2.5 ceiling).
One sample of one route; it could be the mirror now being free to move at rest, or it could be the
route. Not attributed.

**NEXT, in order:** fix A6's before-sample; then find what holds a mirrored rig low — the four rig
bodies' world Z is now logged (`partZ=`) and has not been read yet, and it distinguishes "the whole
rig is low" (support) from "the body hangs in its suspension" (rig state).

### 16.8 `[V]` The audit folded, and the fall has TWO sources — one closed, one open
Post-ship audit (2026-08-30): no CRITICAL, 1 HIGH, 3 MEDIUM. **Three of §16.5's stated properties
did not hold, and two were visible in the run §16.7 reported as evidence.**

| # | what was claimed | what was true |
|---|---|---|
| F1 | — | `restReplaces` was missing from the actor-succession reset (`atv_sync.cpp:374-380`), whose own comment names the reachable path ("a client join runs two level loads"). A successor actor could inherit an exhausted budget and never be corrected again. |
| F2 | "bounded at three re-places, then say so" | Not a bound. The counter cleared on any in-band packet, and a teleport lands the rig exactly in band — so the give-up fired **three times in 46 s** in the shipped run. Now bounded per 10 s EPISODE, and landing in band no longer clears it. |
| F3 | the mirror is not written to when the author is at rest | Defeated by the ANGULAR term alone. `[ATVC] wireLin \|v\|=4.63` — under the linear band — but angular over it routed the packet onto the full write path and the mirror gained **+51 cm/s of Z**. The exact mechanism the fix claimed to remove, live in the run that shipped it. |
| F4 | — | The WARP arm sits ABOVE the at-rest test and still did `TeleportRig` + write, unbounded; and its log line was emitted BEFORE the teleport, so it claimed warps that never happened when `teleportVehicle` was unresolved. |

**The fold made it one rule at every write site instead of one branch.** `WriteMirrorVelocity` skips
the LINEAR component when the author is linearly at rest and writes angular regardless (new
`engine::SetActorRootPhysicsAngularVelocity`) — two quantities, two gates, one place. The
corrective term is governed by the linear gate too, since it is a linear push.

**Two runs on the folded build (`10F32B157948EFCE`), and they disagree:**

| run | A2 | A6 (release) | verdict |
|---|---|---|---|
| 1 | **7.0 cm** | −4.5 (gap closed) | **ACCEPTANCE: PASS** — every arm green, the first time |
| 2 | 39.7 cm | +37.3 | FAIL |

**So the fix is NOT the whole defect, and one green run would have been a false claim.** The
failing run names the second source:

```
[ATVC] NUDGE dist=6.7 cur.z=5482.1 wire.z=5475.5 wireLin=(-2.5,-6.2,-40.9) |v|=41.5
```

The AUTHOR is falling at 41.5 cm/s (Z −40.9) at the moment the client releases. `linAtRest` is
correctly false, so we write that descent onto a mirror that has **already landed**, and a second
later it is 39 cm down. The fall therefore has two sources in two regimes:

- **parked author, `|v| = 0.0`** — our write woke a settled rig. **CLOSED** by the rule above.
- **settling author, `|v| = 41.5` mostly −Z** — we faithfully mirror a real velocity whose effect
  the author has already finished by the time the packet lands. **OPEN.** This is the mechanism
  round 1 proposed and §16.3 recorded as dead: it *is* dead for a parked author and alive for a
  settling one. Recorded so the retraction is not over-read.

Note this is what MTA's asymmetric epsilon is about — `bSyncVelocity`'s Z test is `0.1` against
`FLOAT_EPSILON` for X/Y (`CUnoccupiedVehicleSync.cpp:311`). They widen exactly the axis this
defect lives on. Not ported, not yet designed; the next step is to measure the author's settling
transient (the probe now logs `angv=` as well as `vel=`, F8) rather than to guess a constant.


---

## 17. `[V]` THE SAG WAS OUR OWN COLLISION GUARD — measured 2026-08-30 (autonomous, NOT hands-on)

Since 2026-08-29 a mirrored ATV has settled 25-40 cm below its author in **every run ever
measured**. It was blamed, in order, on the mirror's tick being off (§14 era), on the two peers'
terrain differing under the vehicle (§14.6), on the corrector's gain (§15.2a, §15.5), and on a
velocity write waking a settled body (§16). All four are wrong. It was
`coop::atv_hit_guard` cancelling all seven `ComponentHit` delegates on a non-owner.

### 17.1 The quantity that made it visible
**Ride height** = the body's Z above the mean of its own three rig bodies (`vehicleGetParts`).
Local to one peer, so it survives the peers disagreeing about the world; signed, so "the body is
UNDER its own wheels" is a number rather than an interpretation.

`susFR/susFL/susBK` **cannot express it.** They are 3-D distances from a wheel body to the body
over a ~92 cm *mostly horizontal* arm, so a 40 cm *vertical* deformation moves them by ~1.1 cm —
inside the "2-4 cm of normal suspension travel" band §13 established and A1 asserts. That is why
**A1 passed on six runs in which A2 failed, on the same vehicle**. The lane graded itself green on
a quantity ~36x blind to the only axis that ever failed.

### 17.2 `[V]` The four-cell experiment
Four autonomous smoke runs, host authoring and client mirroring one parked ATV. Archives
`research/atv_runs/20260830-1057*` / `-1059*` / `-1102*` / `-1105*`, all on DLL
**`1407CAF5B2DE6C91`**.

> **CORRECTED 2026-08-30 by the post-ship audit — read this before the table.** Four things in the
> first version of this section were wrong, and the reader should know which:
>
> 1. **It cited DLL `51893CE9`. That is the hash of `20260830-105103`, a BROKEN run** whose client
>    never logged `hit guard armed` and which produces no acceptance section at all. The four cited
>    archives are internally consistent on `1407CAF5B2DE6C91`, so the experiment is not confounded
>    by mixed binaries — the citation simply pointed at the discarded run.
> 2. **Row 1 is not a cell of this experiment.** There is no `corrector ON + guard all seven` run on
>    `1407CAF5`; the 25-40 cm band is a historical range across **seven different binaries** spanning
>    a week of changes. The nearest same-binary candidate, `-1052`, reads A2 = 29.5 FAIL but A7 could
>    not judge it. So the honest claim is a **1×2 pair, not a 2×2**: `-1059` (guard on) vs `-1057`
>    (guard off), same binary, corrector off in both — A2 30.4 → 5.3, rig shape 19.05 → 0.12. That
>    pair alone carries the conclusion, and it carries it cleanly.
> 3. **The row marked "shipped" ran the OLD mask on the host** (`-1105`: host `0x7F`, client `0x03`).
>    It is a valid measurement of a client mirror under mask 3; it is not the shipped configuration.
>    The shipped configuration is `-1111` / `-1113` / `-1140`, both peers `0x03`.
> 4. **The corrector ON/OFF column is recorded in NO log line anywhere.** Its only corroboration is
>    that the claimed-OFF runs have zero client `[ATVC]` lines and the claimed-ON runs have some.
>    That is indirect, and the experiment's headline variable is therefore not archivable. Owed: a
>    one-line `atv: corrector <on|off>` at install.

| corrector | collision guard | A2 settled gap | the two rigs' SHAPES differ by |
|---|---|---|---|
| ON | all seven | 25-40 cm **FAIL** | ~40 cm (six runs, every driven run ever) |
| OFF | all seven | 30.4 cm **FAIL** | 19.05 cm |
| ON | none | 3.0 cm **PASS** | 0.61 cm |
| OFF | none | 5.3 cm **PASS** | 0.12 cm |
| ON | **body delegates only** | 13.6 cm **PASS** | 0.14 cm |

**The pose corrector is INNOCENT** — with the guard off it produces the best cell of the four. Two
days of fixes were aimed at the wrong subsystem, and the reason it looked guilty is that it is the
only *other* thing keyed on authority.

### 17.3 Why the guard did it
The census `atv_hit_guard.cpp` was built on is a list of what each of the seven handlers
**authors** — `impulse()` health and `explode()`, `processTire()` durability and `ejectWheel()`, a
`lib_C::addHint`. It is accurate, and it is not the whole story: the five **wheel** delegates also
maintain the rig's own shape. Cancelling them suppressed a notification carrying two unrelated
things in order to stop one of them, and took the other with it — principle 4, patch the site and
never the class of call.

The bytecode corroborates without settling the mechanism. In `ExecuteUbergraph_ATV`, expr
1228-1229 is `Array_Contains(wheelsOnSurface, ..)` -> `EX_JumpIfNot` guarding expr 1230-1237, which
ends in `mesh.AddForce(GetUpVector * ..)`; the same test at 1198-1202 selects the `SetMassScale`
applied to `backWheelRoot` / `frontWheel_L` / `frontWheel_R`; and `wheelsOnSurface` is written from
inside the wheel-hit segments (exprs 366-379, 577-586, beside `processTire` and `checkAirtime`).
~~**But `wos` reads 4 on BOTH peers at runtime**, including on a not-yet-placed actor — so the array
is not the live contact set that story needs.~~ **RETRACTED 2026-08-30 by the post-ship audit: that
was an INSTRUMENT DEFECT, not a finding.** `wheelsOnSurface` is a `TArray<bool>` whose CDO default
already has four elements, and the probe was reading its **Num** — 4 by construction — rather than
the four values the tick's `Array_Contains` actually tests. Corrected, it reads `wos=0xF` on a
parked ATV: all four wheels on a surface. A `[?]` was minted in this document by the same commit
whose thesis was "the instrument was blind to the axis that failed".

The exact path from "delegate cancelled" to "body 40 cm low" remains `[?]` — the corrected `wos` has
not yet been read on a DEFORMED mirror, which is the one sample that would settle it. **The four-cell
result does not depend on it.**

### 17.4 What shipped (`8cd0ac25`, proto 146 unchanged — no wire change)
The two **body** delegates (`mesh`, `car1_Capsule`) stay cancelled on a non-owner, so damage
authorship and `explode()` are still denied. The five **wheel** delegates run everywhere.
`[dev] atv_hit_guard_mask` (default 3) keeps the experiment re-runnable and `[dev] atv_corrector`
is the control arm that acquitted the corrector; both are diagnostics, RULE-2 exempt.

> **CORRECTED 2026-08-30 by the post-ship audit: the "never" and "always" below were FALSE, and
> the run that falsifies them is one I documented myself the day before.**
> `research/atv_runs/20260830-092139` is a **driven** run with **all seven** delegates cancelled on
> both peers, and it is **the only `ACCEPTANCE: PASS` on disk** — A2 **7.0 cm**, A6 PASS on both
> peers, A5 115.5 cm, A4 clean. So "A2 never passed on a driven run" is false, "A6 was always FAIL"
> is false, and "every driven run" is false: nine archived driven all-seven runs read A2 = 40.2 /
> 25.4 / 30.5 / 38.9 / 36.6 / **7.0** / 39.7 / 29.5 / 30.4 — eight of nine, not nine of nine.
>
> That run already has a lesson written about it, twenty-four hours earlier, saying a single green
> run after a red streak is not a result. Writing "never passed" is the same error inverted: I
> excluded the archived green run because it did not fit, and then used a two-run green streak as
> VERIFIED. **The fix's conclusion does not depend on any of this** — the `-1059`/`-1057` pair does
> — but the magnitude of the improvement is overstated by any "never".

`[V]` Two consecutive verification runs on the shipped default (DLL `2F9A559D`), archives
`-1111*` and `-1113*`, **both of which drove the ATV** (a third, `-1140` on `3A4C2C2A`, reads A2
**1.83 cm** and rig-shape **0.00 cm**, but was not driven):

| arm | run 1 | run 2 | before |
|---|---|---|---|
| A1 rig travel | x0.95 / x1.00 / x1.23 | x0.90 / x0.59 / x2.11 | x2.30 / x2.68 / x1.69 |
| A2 settled gap | 9.59 cm PASS | 3.77 cm PASS | 8 of 9 driven runs FAIL (25-40 cm); one passed at 7.0 |
| A6 handoff | +6.8 / -19.6 PASS | +4.5 / +0.7 PASS | failed in some runs, passed in others |
| A7 rig shape | 2.43 cm PASS | 0.44 cm PASS | (arm did not exist) |
| A3 guard armed | 7/7 both peers | 7/7 both peers | 7/7 |

### 17.5 What is STILL open
- **A5** — the mirror trails the author by up to 209-324 cm while driving. **USER, watching the
  run 2026-08-30: the drive arm leaves the garage and then hits a wooden fence.** That does NOT
  make the number an artefact to discard — it tells us what the number IS, and the corrected
  reading is worse than "noise": `[V]` the author's last 12 driven samples of run `-111354` sit at
  `body=(-3628.., -2155..)` moving at 0.7-5 cm/s — **wedged against the fence with `driven=1` still
  true and the arm still pulsing the throttle**. So A5's window mixes real driving with a long
  stretch in which the author is not moving at all, and a 200 cm gap measured *there* is not a
  trail: it is a STATIC error the corrector should have closed and did not, which A2 cannot see
  because A2 only reads the settled tail after the arm ends. Before A5 is judged, its window must
  be narrowed to samples where the author is actually in motion — and the wedged stretch deserves
  its own arm, because "the mirror sits 2 m from a stationary author" is a distinct defect from
  "the mirror lags a moving one". Same caveat on A1's one x2.11 cell. (The arm already knows the
  base is not a test track — it pulses the throttle and re-seats after a crash, `atv_probe.cpp:44-55`
  — but nothing downstream distinguishes moving from wedged.)
- **A4** — a one-second ownership overlap at the handoff, in both verification runs.
- **The residual this fix creates — and the audit found it is bigger than I stated.** A mirror now
  runs `processTire()`, so it burns its own tire durability and can `ejectWheel()` a tire its author
  still has. I called the fix "tire durability on the wire under the author". That covers the
  `tiresDurability[]` value and **not** the two other things `ejectWheel` does (§2.5): it
  **BeginDeferred-spawns a real `prop_atvWheel_C`** and FinishSpawns it, and it calls `updTires()`,
  which **`BreakConstraint()`s all eight constraints and re-places `sus_*` from
  `defaultTireLocations()`** — a constraint-rig rebuild, on the mirror, i.e. the same class of event
  as the deformation this whole section is about.
  **THE GATING MEASUREMENT IS ANSWERED — 2026-08-30. `[V]`: a runtime-spawned `prop_atvWheel_C`
  DOES carry a Key, and it is minted PER PEER AND AT RANDOM, which makes the defect worse than the
  "ejected twice" I wrote below it.** The mechanism is `[RD]` from bytecode (the table below); the
  FACT is `[V]` from a real field log (the box at the end of this bullet).** The chain, every link
  disassembled with `research/bp_reflection/_fn.py`:

  | step | evidence |
  |---|---|
  | `ATV.ejectWheel` @107-427 | `BeginDeferredActorSpawnFromClass(prop_atvWheel_C)` → writes ONLY `durability`, `dirt`, `fixes` → `FinishSpawningActor`. **No key write, no `loadData`.** |
  | `prop.UserConstructionScript` @63-128 | resolves the gamemode, then calls `init()` (the `resetKey` branch instead sets `key = None`) |
  | `prop.init` @1265-1288 | `getKey(out)` → `key := out` |
  | `prop.getKey` @11-75 | `lib_C::assignKey(key, self, self, out)` → `key := out` |
  | **`lib.assignKey` @81-133** | **`if (keyIn == None) keyIn := generateRandomKey()`**, then @234-771 registers the pair into `gamemode.keyObj_key` / `keyObj_obj` |

  So the UCS mints a key at construction, on whichever peer ran the spawn, before
  `FinishSpawningActor` returns — which is exactly why the seam's drain "adopts ~1 tick later, once
  the whole BP call has completed".

  **Two corrections to what this bullet used to say.** (1) *"carries a Key and so passes
  `IsKeyedInteractable`"* conflated two different gates. `ue_wrap::prop::IsKeyedInteractable` is a
  **CLASS** test — `IsClassKeyedInteractable(R::ClassOf(obj))`, `prop.cpp:154` — so the wheel passes
  it on lineage alone whether or not it holds a key. The instance-key gate is a **separate**
  `keyStr.empty() || keyStr == L"None"` check further down `GrabObserver_Aprop_Init_POST_Body`
  (`prop_lifecycle.cpp:276`), and passing the first tells you nothing about the second. I nearly
  wrote the opposite conclusion off the function's NAME.
  (2) The failure is not "the same wheel ejected twice". Because `generateRandomKey` runs
  independently in each process, **two peers ejecting the same tire mint two DIFFERENT keys** — so
  the identity layer cannot recognise them as one object, and no after-the-fact reconciliation is
  possible. Worse in the ordinary case: the express seam is **host-only**
  (`prop_lifecycle` returns on `role() != Host`), so a client mirror's `ejectWheel` spawns a real,
  keyed, registered wheel that is **broadcast nowhere** — a keyed prop existing on exactly one peer,
  plus a `tires[index] = false` and an `updTires()` constraint-rig rebuild the other peer never sees.
  Neither seam catches it from the other side either: `kAmbientPropSpawnMirrorClasses` is exactly
  three classes (`prop_food_pinecone_C`, `prop_stick_C`, `prop_crystal_C`) and the wheel is not one.

  This CONFIRMS the prescription rather than changing it: the proper root is **an act-as-host intent
  lane for tire ejection** (`COOP_SYNCER_MODEL.md` §2b) — the mirror's `ejectWheel` must not spawn,
  the author's must, and its `prop_atvWheel_C` must be the only one. What the measurement adds is
  *why nothing cheaper works*: with per-peer random keys there is no dedupe to fall back on.
  **RUNTIME-CONFIRMED THE SAME DAY, `[V]`, and it needed no new run — the evidence was already on
  disk in a field report.** `ignore_folder/arigalit_red_mist_desync/multivoid_host.log`
  (**b143**, `compiled Aug 27 2026`) carries the exact event, 47 times:

  ```
  grab_hook[Aprop.Init POST]: HOST broadcasting SPAWN cls='prop_atvWheel_C'
      key='a3sABSN08tUFHU_4LTC2JA' loc=(-2328.6,-1550.1,6103.8) heavy=0 frozen=0
  ```

  **7 distinct wheel keys** in that session, every one a 22-char base64 GUID
  (`a3sABSN08tUFHU_4LTC2JA`, `fx3UaihcpoZhTjy9DJm1hg`, `gqk_ifh3itHph-0si8vlNg`, ...) — which is
  `generateRandomKey` output, so the bytecode chain above is confirmed at runtime. The paired client
  log carries **the same 7 keys and not one key absent from the host's set**, so the host-authored
  eject path broadcasts and adopts correctly, 7 for 7. (The client's higher line count is `hand_item`
  slot-0 mirror spawns — wheels get carried — not extra spawns.)

  **What that log CANNOT test, and why the distinction matters.** b143 was compiled two days BEFORE
  arc 1 (`070c7d29`+`a2a45fc7`, 2026-08-29), i.e. while a mirrored ATV was still frozen and
  teleported rather than simulated. A client mirror on b143 therefore could not run `processTire()`
  at all, so a client-side `ejectWheel` was **structurally impossible** in that build. The absence of
  a client-minted wheel key there is not evidence the divergence does not happen — it is evidence
  that the window did not exist yet.

  **CORRECTED the same day by `/qf` round 1 — the attribution below was wrong, and the commit that
  published it (`a7770193`) is wrong with it.** I wrote that arc 1 introduced this residual on
  2026-08-29. `[V]` `git show 8cd0ac25^:src/votv-coop/include/coop/config/config_registry_rows.inc`
  has **no `atv_hit_guard_mask` row at all**: before today the guard cancelled all seven ComponentHit
  delegates unconditionally, so a mirror's wheel hit never dispatched and **`processTire` could not
  run on a mirror at any point between arc 1 and now**. Arc 1 made the mirror SIMULATE; the guard
  kept the damage lane shut anyway. **The window was opened by `8cd0ac25` — my own commit, today.**
  That makes it live in the tree rather than historical, and still unreleased (field b143, tree
  b146, unpushed), so it can be fixed before it ships — which is the reason to do the lane now.
  The fence collision above is exactly the event that exercises it.
- **§16.6's hook boundary is untouched and still unmeasured**: a player can tie physics props to
  the ATV with `hook_C`, whose lane has zero symbols in this tree.

### 17.7 The tire lane, mapped end to end (2026-08-30, `[RD]` bytecode)

Written before designing the intent lane, because a design brief for a sync defect is worthless
until every event the action emits is mapped on both peers. All offsets from
`research/bp_reflection/` (`_fn.py ATV <fn>`, `ATV_cfg/ATV.txt` for the ubergraph).

**CORRECTED by `/qf` round 1: there are FIVE `processTire` sites, not four, and the fifth is on a
delegate this project CANCELS.** `[V]` `ATV_cfg/ATV.txt` dispatches `processTire` at @15037, @14864,
@9168, @9123 and **@8639**. The fifth reads `K2Node_ComponentBoundEvent_HitComponent_6` /
`NormalImpulse_6`, which by `ue_wrap/devices/atv.cpp:138` is
`BndEvt__car1_Capsule_..._ComponentBoundEvent_6_...` — **`car1_Capsule`, delegate bit 1, one of the
two BODY bits `g_cancelMask` still cancels.** It picks its index with `SelectInt(2, 3, dot > 0)` and
its component with an `EX_SwitchValue` over `backWheel_R`/`backWheel_L`, i.e. a capsule impact is
attributed to a REAR tire. So the shipped guard already suppresses one of the five damage paths on a
mirror, and §5 / §17's description of the two BODY cancels as covering "impulse-damage and
`explode()`" is incomplete — they also cut a tire-damage path. **Four paths remain live on a
mirror**, not five.

```
ComponentHit on a WHEEL  ->  ubergraph wheel segment (four copies, e.g. @14864 idx3, @15037 idx2)
    1. processTire(index, HitComponent, NormalImpulse)
    2. checkAirtime()
    3. wheelsOnSurface[index] = true
    4. RetriggerableDelay(0.1s) -> latent -> wheelsOnSurface[index] = false

processTire  (42 stmts)
    sev := VSize(impact / mesh.GetMass()) / 100 / 1.5
    if (sev > 1.0)  -> damageWheel(index, sev, component)
    else            -> the dirt branch (dirtVel lerp, accumulates tire dirt)

damageWheel  ->  ExecuteUbergraph_ATV(15210)  ->  switch on index, four blocks
                 (@15479 / @15840 / @16160 / @16470)
    tiresDurability[index] -= damage        (VictoryFloatMinusEquals -> FloatOut)
    tiresDurability[index]  = FMax(FloatOut, 0)
    if (FloatOut <= 0)  ->  ejectWheel(index, component)   @15653
    sound_tireDamage(component)                            @15685

ejectWheel   ->  BeginDeferred(prop_atvWheel_C) + durability/dirt/fixes-1 + FinishSpawning
                 + copy the wheel component's lin/ang velocity onto the spawned mesh
                 + tires[index] = false + updTires()  (BreakConstraint x8, re-place sus_*)
```

**Two things this map settles.**

**(a) It confirms the collision-guard mechanism, which §17 had only by correlation.** §17 shipped
"the five WHEEL delegates also keep the rig's SHAPE" on a measured pairing (`-1059` vs `-1057`)
without naming the path. It is step 3 above: a wheel `ComponentHit` is the ONLY writer of
`wheelsOnSurface[index]`, and `wheelsOnSurface` is what gates the ubergraph's suspension `AddForce`
(exprs 1228-1237) and `SetMassScale` (1198-1202). Cancel the wheel delegates on a mirror and that
array never goes true, so the up-force never runs and the body settles under its own wheel plane —
exactly the 25-40 cm sag. Correlation upgraded to mechanism; the shipped fix is right for the reason
now written down.

**(b) The tire lane is not a new problem — it is `docs/COOP_WORLD_PROP_DIVERGENCE.md` applied to a
vehicle.** Its shape is that doc's shape exactly: the INPUT is a physics impulse (per-peer, and the
two peers' impulses are not equal), the ACCUMULATOR is a plain local float array
(`tiresDurability[]`) mutated over time, and the OUTPUT is a THRESHOLD crossing (`<= 0`) with an
irreversible side effect. Since arc 1 the mirror simulates, so both peers accumulate independently
and cross that threshold at different moments — or only one crosses it. The documented root for that
class is the same one §17.5 arrives at from the identity side: **the host owns the progression**.
Two independent routes to one answer is the strongest signal available here.

**The inverse half, mapped the same day — and both halves change the design for the better.**

```
putTire(index, wheelObject)   (15 stmts)
    if (index < 0)      -> addHint, return
    if (tires[index])   -> addHint, return        // slot already occupied
    tires[index]          = true
    tiresDurability[index] = wheelObject.durability
    tiresDirt[index]       = wheelObject.dirt
    tiresFixes[index]      = wheelObject.fixes
    updTires()
    wheelObject.K2_DestroyActor()                 // consumes the keyed prop

updTires()   (204 stmts, 7987 bytes)
    setWheelsType()
    per wheel, driven ONLY by tires[i]:
        frontWheel_R.SetVisibility(tires[0]) / SetCollisionEnabled / SetCollisionResponseToChannel
        tirePoint_FR.SetCollisionEnabled(...)
    sus_BR1.BreakConstraint() ... (the rig re-place)
```

**(c) The wheel prop IS the tire's state carrier.** `ejectWheel` writes `durability` / `dirt` /
`fixes-1` onto the spawned actor and `putTire` reads exactly those three back into the arrays. The
game already serialises tire state through the prop, so a sync design does not need to invent a
transfer format — it needs to make sure ONE peer authors the prop. It also means the destroy half
already has a wire path: `putTire` ends in `K2_DestroyActor`, the ordinary observable destroy seam,
not a BP-internal disappearance.

**(d) `updTires()` is a pure REDUCER over `tires[]`, so the rig does not need syncing — the array
does.** Every visibility, collision and constraint call in those 204 statements is derived from
`tires[i]`; nothing in it reads a hit, an impulse or a timer. So it is idempotent with respect to the
array, and two peers holding the same `tires[]` who both call `updTires()` end with the same rig.
The design consequence is large: **do not mirror `BreakConstraint`/`SetCollisionEnabled` operations
— mirror the four arrays and call the game's own reconciler.** That is `park the brain, drive the
entity` in its correct form, and it is the opposite of what the C1 crutch did to this vehicle.

**Still unmapped (small, and not on the critical path):** `updSpareTire`, `diretTire`,
`setWheelsType`'s type table, and whether `tiresTypes[]` rides any existing wire lane.

### 17.8 `/qf` round 1 killed the first design, and the replacement is cheaper (2026-08-30)

The design taken into the round was: suppress the damage lane INSIDE
`processTire`/`damageWheel` on a non-author, push four arrays, make eject/put intents.
**Pillar 1 is unbuildable and the round proved it in one line.**

**`[V]` every one of the five `processTire` sites is `EX_LocalVirtualFunction`, and `damageWheel` is
an ubergraph thunk.** Neither is visible to our ProcessEvent detour (`COOP_DISPATCH_VISIBILITY.md`
— the `init()`-is-BP-internal trap, which CLAUDE.md's own reading order exists to prevent). The only
substrate that sees `EX_Local*` is the `0x45` GNatives swap in `COOP_VM_DISPATCH_PLAN.md`, which is
HALT-gated and unbuilt. A design cannot name a suppression point we have no way to reach.

**The replacement inverts it: do not suppress the accumulation at all.** A mirror accumulating its
own `tiresDurability[]` is harmless *if it is continuously overwritten*, and §17.7(d) already
measured that `updTires()` is a pure reducer over plain array properties — so the author's on-change
push simply corrects the mirror, no interception required. The ONLY irreversible act is
`ejectWheel`'s spawn, and that one IS reachable: `BeginDeferredActorSpawnFromClass` issued from a BP
ubergraph is `EX_CallMath` (invisible to PE) but is **caught by the shipped `UFunction::Func` thunk
patch** (`ue_wrap/ufunction_hook`, `d19ae4d4`), already proven on the pile morph, the 32-wisp swarm
and `piramidSpawner_C`. **One measurement gates this and is NOT done: can that thunk patch CANCEL a
call, or only observe it?** Until that is answered the replacement is a sketch, not a design.

**The authority fork is resolved, and not by the doc I was following.** I reached for
`COOP_WORLD_PROP_DIVERGENCE`'s "the host owns the progression" after classifying tires as a
self-simulating prop — i.e. I searched prior art by the mechanism I had assumed rather than by the
problem, the exact failure `[[lesson-search-prior-art-by-problem-not-by-assumed-mechanism]]` is named
for. Grepping MTA for *vehicle tyre damage* instead: `CDeathmatchVehicle::SyncDamageModel:43-120` is
this design's step 2 verbatim (per-wheel change-edge diff feeding `SetWheelStatus`), and
`CClientVehicle::CalcAndUpdateTyresCanBurstFlag:1252-1270` settles the ownership question as
**`local driver || syncing-unoccupied`, never the server**. Per RULE 2026-05-28 that outranks the
world-prop doc's answer: **the ATV's syncer owns tyre damage, not the host.**

MTA's *mechanism* does NOT port, and that is measured rather than assumed: it suppresses bursting by
setting a native capability flag (`SetTyresDontBurst`) instead of hooking anything, and a census of
`ATV_C`'s **1,527 distinct properties** finds no equivalent — no burst, invulnerability or
tire-capability flag exists on this class (`ignoreFallDamage` is the nearest and is unrelated). So
the shape ports and the trick does not.

**MEASUREMENT (1) RAN THE SAME HOUR AND KILLED THE REPLACEMENT AS STATED. `[V]` the `Func` patch
cannot cancel — it is POST-ONLY BY CONSTRUCTION.** `ue_wrap/core/ufunction_hook.h:59` types the
callback `using PostNativeCallback = void(*)(void*, void*, void*)` — a `void` return, so there is no
value with which to refuse — and the header states the forwarder "forwards to the original Func
(which steps the params ... + runs the impl + writes `*Result`), THEN reads `*Result`". The original
always runs. There is no pre-hook and no skip in the facility at all.

That same header carries a warning worth repeating here, because it independently confirms the
finding that killed pillar 1 and describes a failure that would have looked like success: a Func
patch on a **script** UFunction reached via `EX_Local*` **installs successfully, logs "patched", and
never fires** (`Func` = `ProcessInternal`, non-null, so it passes the null guard). Both dispatch
handlers branch on `FUNC_Native`; only a native callee reads `Func`. `processTire` is script.

**So the third design of this pass, and the first one every link of which is measured: DESTROY AT
BIRTH, ONE TICK LATER.** `BeginDeferredActorSpawnFromClass` and `FinishSpawningActor` are NATIVE, so
the Func patch does fire for them and hands the callback `sourceObject` (= `FFrame::Object`, the ATV
whose ubergraph is executing) and `spawnedResult` (= the new wheel). If that ATV is not ours, the
wheel is ours to remove. It must NOT be removed inside the callback: `ejectWheel` continues after
`FinishSpawningActor` to call `SetPhysicsLinearVelocity` / `SetPhysicsAngularVelocityInDegrees` on
the spawned actor's `StaticMesh`, so destroying mid-BP hands the rest of the function a dead actor.
Defer by one tick — which is exactly the precedent `host_spawn_watcher::DrainPendingSpawns` already
sets and already explains ("the callback only ENQUEUES; DrainPendingSpawns adopts ~1 tick later, once
the whole BP call ... has completed"). The local `tires[i] = false` and `updTires()` still run on the
mirror and are corrected by the author's array push, which the lane needs anyway.

**The general answer for the whole class remains the `0x45` GNatives swap** (`COOP_VM_DISPATCH_PLAN`),
which would make `processTire` and every other `EX_Local*` verb interceptable and close this defect at
the root rather than at its one irreversible side effect. It is named here rather than dismissed: the
radical mandate makes its size a non-reason. It is not proposed for THIS lane because destroy-at-birth
is complete for the observable defect and the swap is HALT-gated on a spike this lane does not gate.

**Still owed:** (2) a sample from `research/atv_runs/` where a MIRROR actually reaches `sev > 1.0`
(`|impulse|/mass > 150`) against 100 starting durability — **the defect is derived from bytecode and
has never been observed**, which is the weakest link in the whole case; (3) `putTire`'s intent shape
against `COOP_SYNCER_MODEL` §2b; (4) whether destroy-at-birth needs to suppress the mirror's
`sound_tireDamage` too, or whether a phantom pop is acceptable.

### 17.6 Three things not to re-derive
1. **Deleting the tick-off (`a2a45fc7`) did not move the number.** All six runs that read 25-40 cm
   are post-deletion; the pre-arc-1 measurement recorded in `atv_hit_guard.cpp` read 37 cm. Two
   different mechanisms, one identical outcome, because the guard shipped *with* arc 1 and was
   never in the frame. The attribution of that 37 cm to a skipped `SetCenterOfMass` is **falsified**.
2. **The corrector's give-up WARN named the wrong causes.** It blamed the terrain under the vehicle
   or an unseen constraint such as a hook. The wheels of the two copies agree to <=1 mm at the same
   XY, so the terrain is identical, and the real cause was in our own process.
3. **The archive filter dropped every `[ATVC]` line** for its whole life, so §16's quoted wire
   velocities were only ever reproducible from a live log the next run overwrites — and one of them
   is now permanently gone (another session overwrote the host log at 09:45). Archive a run the
   moment it ends.
