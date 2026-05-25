# RE: VOTV Doors and Light Switches — Phase 5D RE
**Date**: 2026-05-25
**Target**: Alpha 0.9.0-n
**Method**: CXXHeaderDump static analysis + cross-reference with mainGamemode.hpp, triggerBase.hpp, lib.hpp. No IDA or UE4SS Lua probing performed yet (open flags listed in Section 8).

---

## Section 1 — Class Enumeration

### 1.1 Door Classes

**`Adoor_C`** — primary interactable door
- File: [door.hpp:4](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/door.hpp#L4)
- Parent: `AtriggerBase_C`
- Size: 0x42D
- This is the only concrete door class used for regular interior/exterior passage doors. It is a double-leaf sliding/swinging door: `door_L` and `door_R` are `UStaticMeshComponent*` (the two panels), animated by three `UTimelineComponent*` members (`Timeline_0`, `openJammed`, `move`). The `faceSmasher` `UBoxComponent` implies there is a collision zone that triggers bump-open.

Key fields (all offsets confirmed from header):

```
isOpened     bool  0x0350  — canonical open/closed state
isMoving     bool  0x0351  — in-animation flag
Active       bool  0x0352  — whether the door is interactive (false = locked out)
autoclose    bool  0x0353  — auto-close after open
superClosed  bool  0x0378  — override: prevents ANY opening (script-locked)
ignoreNPC    bool  0x0388  — when true, the sensor does NOT auto-open for NPCs
jammed       bool  0x03B0  — blocked open (partially open, cannot close)
alienated    bool  0x03B1  — an alien-effect state (exact semantics unclear, RE flag E-D1)
keycardName  FString 0x03B8 — non-empty = requires keycard with this name
passlocks    TArray<ApasswordLock_C*> 0x0390 — linked password locks
cantClose    bool  0x042A  — another close-prevention flag (collision-driven)
ignoreBlackout bool 0x03C8 — door stays powered during blackout
damageProtected bool 0x0410
dir          TEnumAsByte<ETimelineDirection::Type> 0x0428 — current animation direction
```

Timeline members (three independent timelines):
- `Timeline_0` (0x0328): the main open/close rotation timeline. The `a` float (0x0320) is the alpha (0.0 = closed, 1.0 = open).
- `openJammed` (0x0338): the jam-open animation.
- `move` (0x0348): a secondary movement component (likely the sliding portion for wide doors or the door glide into wall).

The `openDoor` `FVector` (0x03E0) and `lerpA_R`/`lerpA_L` FVectors suggest direct lerp-driven panel transforms rather than a skeletal animation. Both panels are independently positioned: the timeline alpha drives a LERP between closed and `openDoor` target positions for each panel.

UFunctions relevant for coop:
- `void doorOpen(bool bypassCheck)` — opens the door; `bypassCheck` skips jammed/locked guards. This is the canonical "open" entry point.
- `void doorClose(bool bypassCheck)` — closes the door.
- `void player_use(AmainPlayer_C* Player, FHitResult Hit)` — E-press dispatch from mainPlayer interaction system.
- `void checkSensor()` — reads `sensorOverlaps`, opens/closes based on occupancy.
- `void hack(FVector Source)` — bypass open by hacking (script trigger).
- `void jam(bool autoCancel)` — sets `jammed = true`.
- `void jamCancel(float Duration)` — timer-cancels a jam.
- `void runTrigger(AActor* Owner, int32 Index)` — AtriggerBase event-chain handler.
- `void getTriggerData(Fstruct_triggerSave& Data)` / `void loadTriggerData(...)` — **save/load hooks present** (door state is save-persistent).
- `void settime(bool opened)` — snaps the timeline to fully open or fully closed without animation (teleport-to-state).

Delegate: `Fdoor_CDoorOpened doorOpened` (0x03D0, size 0x10) — a dynamic multicast delegate that fires when the door finishes opening. Anything holding a reference can bind to this.

---

**`Adoor_pryable_C`** — crowbar-pryable door (subclass of `Adoor_C`)
- File: [door_pryable.hpp:4](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/door_pryable.hpp#L4)
- Parent: `Adoor_C`
- Size: 0x448
- Adds two `UChildActorComponent* crowbar1/crowbar2` (0x0438, 0x0440). These are embedded `ApryingCrowbar_C` actors embedded in the door itself, placed on the frame.
- Adds `void crowbarOpen(ApryingCrowbar_C* pryingCrowbar)` — the crowbar calls back into the door when its mini-game succeeds to force-open.
- Inherits all `Adoor_C` interaction paths; crowbar is an additional "use" surface.

---

**`Unav_door_C`** — navigation area for door opening
- File: [nav_door.hpp:4](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/nav_door.hpp#L4)
- Parent: `UNavArea`
- Size: 0x48
- This is a navmesh area class (a `UNavArea` subclass, not an `AActor`). It defines the navigation cost/policy for the space a door occupies. Not directly synchronized — it is driven by the navmod child actor embedded in `Adoor_C` (`navmod` `UChildActorComponent* 0x02E8`, child is `AnavModifierBox_C`). The door's navmod controls navmesh passability as the door opens/closes. Not an actor to track for coop sync.

---

**`ApasswordLock_C`** — keypad / keycard lock linked to a door
- File: [passwordLock.hpp:4](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/passwordLock.hpp#L4)
- Parent: `AtriggerBase_C`
- Size: 0x393
- Holds a direct `Adoor_C* door` (0x0338) + `FName door_key` for the key-lookup fallback.
- State: `bool Active` (0x0330 — whether the lock is armed), `bool isAcc` (0x037C — accepted, i.e. unlocked), `bool isCard` (0x0390 — uses card reader vs keypad). Also `FString password` (0x0350).
- `void Open(bool Active)` — programmatic unlock command.
- `void SetActive(bool isPairCall)` — arms/disarms the lock (paired lock support: `Pair` at 0x0320 links two locks on the same door).
- `void loadTriggerData`/`getTriggerData` — save-persistent.
- For coop: a password input or card swipe on either peer must propagate the lock state change. The lock directly calls its stored `door->doorOpen()` when authentication succeeds. Sync requirement: lock state (`Active`, `isAcc`) + door state.

---

**`AcargoliftDoor_C`** — cargo lift door assembly
- File: [cargoliftDoor.hpp:4](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/cargoliftDoor.hpp#L4)
- Parent: `AActor`
- Size: 0x240
- A pure assembly actor: two `UChildActorComponent* door_L/door_R`. Each child is presumably an `Adoor_C` instance. No additional state or UFunctions; it is a prefab container. The actual door logic lives in the child `Adoor_C` actors. The `AcargoLift_C` (0x2F0, `Aactor_save_C` → `AActor`) contains this via `UChildActorComponent* lift` (0x0288). Not directly a sync target; sync the embedded `Adoor_C` children via the standard path.

---

**`ApowerControl_C`** — power control board (affects doors and lights)
- File: [powerControl.hpp:4](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/powerControl.hpp#L4)
- Not a door class itself but notable: `TArray<Adoor_C*> doorsOpen` (0x03C8) — when the power board activates, it iterates this array and calls `doorOpen(bypassCheck=true)` on each. Also `TArray<Atrigger_lightRoot_C*> lighRoots` (0x0388) to control light groups. Relevant because power-board interaction must sync to both peers so the doors open correctly on both sides.

---

### 1.2 Light Switch Classes

**`Alightswitch_C`** — wall-mounted light toggle switch
- File: [lightswitch.hpp:4](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/lightswitch.hpp#L4)
- Parent: `AtriggerBase_C`
- Size: 0x2B0
- Key fields:
  ```
  bool A          0x02A0  — current switch state (on=true / off=false)
  AtriggerBase_C* Trigger 0x02A8  — the trigger this switch fires when toggled
  ```
- The switch does NOT directly reference any lamp actors. It fires its `Trigger` (which is a `Atrigger_lightRoot_C` per level placement) via `AtriggerBase_C::fireTrigger()` or `runTrigger()`.
- `void use()` — the internal toggle called by `player_use`. Flips `A`, then fires `Trigger`.
- `void player_use(AmainPlayer_C* Player, FHitResult Hit)` — E-press dispatch.
- `void gatherDataFromKeyT(bool& gather)` is present (inherits from triggerBase pattern) but no `getTriggerData`/`loadTriggerData` overrides. This means the switch's own `bool A` state is NOT independently save-persistent via the trigger-save system.

Critical implication: the light state is saved by the lamps (`AceilingLamp_C` has `getTriggerData`/`loadTriggerData`), not by the switch. The switch's `bool A` drifts after load unless the game reinitializes it from the lamp state. For coop, we sync the lamp's `IsActive` field (the authoritative state), not the switch's `A`.

---

**`Atrigger_lightRoot_C`** — light group controller (the switch's actual target)
- File: [trigger_lightRoot.hpp:4](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/trigger_lightRoot.hpp#L4)
- Parent: `AtriggerBase_C`
- Size: 0x2E1
- Key fields:
  ```
  TArray<AceilingLamp_C*> lights   0x0298  — all lamps in this group
  TArray<AambientLight_C*> ambs    0x02A8  — ambient rect lights in this group
  bool IsActive   0x02B8  — current active state
  bool Active     0x02B9  — (likely: desired state, pending apply — confirm via probe)
  TArray<FName> lights_keys  0x02C0
  TArray<FName> ambs_keys    0x02D0
  bool buffIsActive  0x02E0  — buffered/deferred state
  ```
- UFunctions:
  - `void SetActive(bool Active)` — the canonical "turn this group on/off". Iterates `lights` array calling `AceilingLamp_C::SetActive(bool)` on each, and iterates `ambs` calling `AambientLight_C::SetActive(bool)`.
  - `void flick()` — momentary flicker effect.
  - `void updLig()` — refresh all lamps from `IsActive`.
  - `void getTriggerData(Fstruct_triggerSave& Data)` / `void loadTriggerData(...)` — save-persistent.
  - `void runTrigger(AActor* Owner, int32 Index)` — receives the fireTrigger from lightswitch (through triggerBase chain).

---

**`AceilingLamp_C`** — individual ceiling-mounted light fixture (the actual light source)
- File: [ceilingLamp.hpp:4](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/ceilingLamp.hpp#L4)
- Parent: `AtriggerBase_C`
- Size: 0x2E8
- Key fields:
  ```
  bool IsActive    0x02C0  — on/off state
  float power      0x02C4  — current power level
  bool hasPower    0x02C8  — grid power available
  float Radius     0x02CC
  float Strength   0x02D0
  float strengthMult 0x02D4
  bool isBlinking  0x02D8
  Atrigger_lightRoot_C* belongsToRoot 0x02E0  — back-pointer to owning group
  ```
- Components: `UPointLightComponent* PointLight` (0x02A8), `UParticleSystemComponent* eff_flare` (0x0298), `UAudioComponent* Audio` (0x02A0), `UStaticMeshComponent* Mesh` (0x02B8).
- UFunctions:
  - `void SetActive(bool IsActive)` — THE canonical on/off entry. Sets `IsActive`, enables/disables `PointLight`, plays particle + audio.
  - `void toggle()` — flips `IsActive` and calls `SetActive`.
  - `void off()` — unconditional off.
  - `void getTriggerData(Fstruct_triggerSave& Data)` / `void loadTriggerData(...)` — save-persistent (lamps save their `IsActive` state).
  - `void powerChanged(bool active_calc, bool active_downl, bool active_coords, bool active_play, bool active_light)` — called by `ApowerControl_C` when power grid state changes.
  - `void blink(float Weight)` — blinking effect driven by the game's atmosphere system.

---

**`AceilingLamp_hanging_C`** — hanging variant of ceiling lamp
- File: [ceilingLamp_hanging.hpp:4](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/ceilingLamp_hanging.hpp#L4)
- Parent: `AceilingLamp_C`
- Size: 0x2E8 (identical to parent)
- Empty body — no additional fields or functions. Pure visual variant. Sync path identical to `AceilingLamp_C`.

---

**`AlightOut_floor_C`** — floor-mounted "lightOut" fixture
- File: [lightOut_floor.hpp:4](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/lightOut_floor.hpp#L4)
- Parent: `AceilingLamp_C`
- Size: 0x2E8
- Only adds `void setVis()` (likely updates mesh visibility based on `IsActive`). Full lamp sync path inherited.

**`AlightOut_ceil_C`** — ceiling-mounted "lightOut" fixture
- File: [lightOut_ceil.hpp:4](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/lightOut_ceil.hpp#L4)
- Parent: `AceilingLamp_C`, Size: 0x2E8, same shape.

**`AlightOut_wall_C`** — wall-mounted "lightOut" fixture
- File: [lightOut_wall.hpp:4](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/lightOut_wall.hpp#L4)
- Parent: `AceilingLamp_C`, Size: 0x2E8, same shape.

**`AlightOut_floor_server_C`** — server-room floor fixture (subclass of floor variant)
- File: [lightOut_floor_server.hpp:4](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/lightOut_floor_server.hpp#L4)
- Parent: `AlightOut_floor_C`, Size: 0x2E8.

All four `lightOut_*` variants: identical size as `AceilingLamp_C`, only `setVis()` added. All use `AceilingLamp_C::SetActive(bool)` as the sync entry point. Treated identically in coop.

---

**`AambientLight_C`** — area ambient rect-light (grouped with lamp roots)
- File: [ambientLight.hpp:4](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/ambientLight.hpp#L4)
- Parent: `AtriggerBase_C`
- Size: 0x2C9
- Key fields: `bool Enabled` (0x02B5), `float Intensity` (0x02B0), components `URectLightComponent* topLight/bottomLight`.
- `void SetActive(bool Active)` — on/off entry; sets `Enabled`, modifies `Intensity`.
- `void getTriggerData`/`loadTriggerData` — save-persistent.
- Referenced from `Atrigger_lightRoot_C.ambs` array. Sync path: `Atrigger_lightRoot_C::SetActive(bool)` already drives these via the `ambs` array iteration.

---

**`AlampPost_C`** — outdoor lamp post
- File: [lampPost.hpp:4](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/lampPost.hpp#L4)
- Parent: `AActor`
- Size: 0x259
- `bool isOn` (0x0248) — state field. `void upd()` — refresh visuals. `void blink(float Weight)` — atmosphere blink.
- This is NOT part of the `Atrigger_lightRoot_C` / `AceilingLamp_C` network. It is driven by the game's day/night cycle (mainGamemode tracks `allLampPosts`). NOT player-interactable. No sync needed — both peers run the same day/night cycle locally.

---

### 1.3 Summary Table

| Class | Parent | File | Coop-relevant? |
|---|---|---|---|
| `Adoor_C` | `AtriggerBase_C` | door.hpp | YES — primary |
| `Adoor_pryable_C` | `Adoor_C` | door_pryable.hpp | YES — inherits |
| `Unav_door_C` | `UNavArea` | nav_door.hpp | NO — navmesh area, not actor |
| `ApasswordLock_C` | `AtriggerBase_C` | passwordLock.hpp | YES — lock state |
| `AcargoliftDoor_C` | `AActor` | cargoliftDoor.hpp | Indirect — contains Adoor_C children |
| `Alightswitch_C` | `AtriggerBase_C` | lightswitch.hpp | YES — switch |
| `Atrigger_lightRoot_C` | `AtriggerBase_C` | trigger_lightRoot.hpp | YES — group controller |
| `AceilingLamp_C` | `AtriggerBase_C` | ceilingLamp.hpp | YES — per-lamp state |
| `AceilingLamp_hanging_C` | `AceilingLamp_C` | ceilingLamp_hanging.hpp | YES — same path |
| `AlightOut_floor_C` | `AceilingLamp_C` | lightOut_floor.hpp | YES — same path |
| `AlightOut_ceil_C` | `AceilingLamp_C` | lightOut_ceil.hpp | YES — same path |
| `AlightOut_wall_C` | `AceilingLamp_C` | lightOut_wall.hpp | YES — same path |
| `AlightOut_floor_server_C` | `AlightOut_floor_C` | lightOut_floor_server.hpp | YES — same path |
| `AambientLight_C` | `AtriggerBase_C` | ambientLight.hpp | YES — driven by root |
| `AlampPost_C` | `AActor` | lampPost.hpp | NO — day/night driven |

---

## Section 2 — Door Interaction Paths

### 2a. E-Press Path

`AmainPlayer_C` has a rich interaction dispatch. The relevant chain for door interaction:

1. Player presses E. `AmainPlayer_C::input_E(bool Pressed)` fires (declared at [mainPlayer.hpp:187](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/mainPlayer.hpp#L187)).
2. `mainPlayer_C` performs an arm trace (`bool useArm(FHitResult& OutHit)`) at [mainPlayer.hpp:468](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/mainPlayer.hpp#L468). This resolves `lookAtActor` + `lookAtComponent`.
3. The trace result is dispatched through the interaction interface. The player calls `playerUsedOn(Player, Hit, lookAtComponent, holdObject, holdPropName)` on `lookAtActor` (the door). This is the `AtriggerBase_C` interface method at [triggerBase.hpp:88](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/triggerBase.hpp#L88).
4. `Adoor_C::player_use(AmainPlayer_C* Player, FHitResult Hit)` is the door's override of the `player_use` UFunction at [door.hpp:99](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/door.hpp#L99). Its BP body (unconfirmed without UE4SS probe — RE Flag E-D2): reads `isOpened`, calls `doorOpen(bypassCheck=false)` if closed or `doorClose(bypassCheck=false)` if open. The `processKeys` step (inherited from triggerBase) validates keycard/password requirements before proceeding.
5. `doorOpen(bypassCheck)` ([door.hpp:118](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/door.hpp#L118)): sets `isOpened = true`, sets `dir` to Forward, starts `move` timeline (`move.PlayFromStart()`). Timeline `move__UpdateFunc` fires per-tick while animating, driving `door_L`/`door_R` positions via LERP between `lerpA_L`/`lerpA_R` (closed) and `openDoor` target (open). `move__FinishedFunc` fires on completion — likely sets `isMoving = false` and fires the `doorOpened` delegate.

Key hook point for coop: `Adoor_C::doorOpen(bool bypassCheck)` and `Adoor_C::doorClose(bool bypassCheck)`. These are the entry points for both E-press and NPC-auto-open; hooking both gives complete coverage.

### 2b. NPC Auto-Open Path

`Adoor_C` has a `UBoxComponent* sensor` (0x0308) — a trigger volume in front of/around the door. This component has two registered overlap handlers:

- `BndEvt__door_sensor_K2Node_ComponentBoundEvent_2_...BeginOverlap...` at [door.hpp:126](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/door.hpp#L126) — fires when any actor enters the sensor volume.
- `BndEvt__door_sensor_K2Node_ComponentBoundEvent_3_...EndOverlap...` at [door.hpp:127](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/door.hpp#L127) — fires when actors leave.

Both update `sensorOverlaps` (TArray<AActor*> 0x0418), then call `checkSensor()` at [door.hpp:116](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/door.hpp#L116).

`checkSensor()` (BP body, unconfirmed — RE Flag E-D3): if `sensorOverlaps.Num() > 0` AND `!ignoreNPC` AND `!superClosed`, calls `doorOpen(bypassCheck=false)`. If `sensorOverlaps.Num() == 0` AND `autoclose`, calls `doorClose(bypassCheck=false)`.

NPC auto-open mechanism: NPCs are `ACharacter` subclasses with `UCharacterMovementComponent`. They physically navigate through the door sensor volume as they pathfind. No `AIPerceptionComponent`, no behavior tree task, no explicit "open door" AI action found in the NPC headers examined — the door itself opens reactively on sensor overlap. The `ignoreNPC` flag on the door allows some doors to be NPC-blocking (e.g., player-only access doors).

Important finding: **player-E-open and NPC-auto-open both funnel through the same `doorOpen(bypassCheck)` entry point**. The two paths are NOT separate UFunction chains. This is the key hook/invoke point for coop.

### 2c. Canonical Open/Close UFunction

**`Adoor_C::doorOpen(bool bypassCheck)`** — hook this POST to detect any open intent on the sender side (fires regardless of cause: E-press, NPC overlap, script/hack). Invoke this on the receiver side to apply a peer's open event.

**`Adoor_C::doorClose(bool bypassCheck)`** — same for close events.

The `bypassCheck` param: when invoking on the receiver, pass `bypassCheck=true` so the receiver doesn't re-run keycard/password validation (the sender has already validated). This prevents a desync where the receiver has different lock state from the host.

### 2d. Bidirectional Doors, Locking, Animation

**Bidirectional**: The `dir` field (TEnumAsByte<ETimelineDirection::Type> 0x0428) indicates animation direction (Forward/Backward). There is no separate "which way the door swings" spatial flag in the header — the single timeline drives both open and close directions. No explicit "left-push vs right-push" field found; the door does not have directional variants from the player's approach. The `faceSmasher` UBoxComponent (0x02D0) causes a `doorImpact(float B)` callback if the player's body intersects the panel mesh during movement.

**Locking mechanisms** confirmed in header:
- `keycardName FString 0x03B8` — non-empty string = requires keycard. `processKeys()` checks this before allowing open.
- `TArray<ApasswordLock_C*> passlocks 0x0390` — linked keypads that must be in `isAcc=true` state for the door to open.
- `jammed bool 0x03B0` — special partially-open state; `openJammed` timeline drives this. Can be cancelled by `jamCancel`.
- `superClosed bool 0x0378` — script-level override preventing any open.
- `alienated bool 0x03B1` — unknown (RE Flag E-D1); probably an alien-event state that overrides normal interaction.

**Animation**: The `move` UTimelineComponent drives the open/close. The alpha float `move_a_*` ranges 0.0→1.0. The update function lerps door panel positions. This is NOT skeletal animation; it is direct component-transform manipulation. On the receiver, calling `doorOpen(bypassCheck=true)` will start the same timeline locally — the animation plays identically on both peers without additional bone/transform streaming. No receiver-side post-processing needed beyond the `doorOpen`/`doorClose` call.

The `settime(bool opened)` function allows instant state-snap (no animation): useful for snapshot-on-connect (Phase 5S0) to restore door state without playing the animation mid-session.

### 2e. Save Persistence

`Adoor_C` implements `getTriggerData` (line 86) and `loadTriggerData` (line 85), both returning/accepting `Fstruct_triggerSave`. The `Fstruct_triggerSave` (struct_triggerSave.hpp) carries `TArray<bool> bools` — the door almost certainly stores `isOpened` here. The `Key` FName (inherited from triggerBase at 0x0260) identifies this trigger in the save map.

This means door state is save-persistent: when the host loads the save (snapshot-on-connect, Phase 5S0), doors are already at the correct open/closed state. The client receives the full host save, so initial door state is covered by Phase 5S0 without any additional door-specific snapshot.

---

## Section 3 — Light Switch Interaction Paths

### 3a. E-Press Path

1. Player presses E near a light switch. `AmainPlayer_C::input_E` → arm trace → `lookAtActor` = `Alightswitch_C` actor.
2. `Alightswitch_C::player_use(AmainPlayer_C* Player, FHitResult Hit)` fires at [lightswitch.hpp:31](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/lightswitch.hpp#L31).
3. BP body of `player_use` (unconfirmed — RE Flag E-L1): presumably calls `use()` directly.
4. `Alightswitch_C::use()` at [lightswitch.hpp:49](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/lightswitch.hpp#L49): flips `bool A` (0x02A0), then fires `Trigger` (the stored `AtriggerBase_C*` at 0x02A8) via `AtriggerBase_C::fireTrigger(this, index)` or `runTrigger`. The trigger chain routes to `Atrigger_lightRoot_C::SetActive(bool)` with the new value of `A`.
5. `Atrigger_lightRoot_C::SetActive(bool Active)` at [trigger_lightRoot.hpp:19](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/trigger_lightRoot.hpp#L19): sets `IsActive = Active`, iterates `lights` array calling `AceilingLamp_C::SetActive(Active)` on each entry, iterates `ambs` calling `AambientLight_C::SetActive(Active)`.
6. `AceilingLamp_C::SetActive(bool IsActive)` at [ceilingLamp.hpp:25](../../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/ceilingLamp.hpp#L25): sets `IsActive` field (0x02C0), enables/disables `PointLight`, plays audio and flare particle.

### 3b. State Fields

The authoritative state is `AceilingLamp_C::IsActive` (bool, 0x02C0). This is what the save system serializes (via `getTriggerData`/`loadTriggerData`). The switch's `bool A` (0x02A0) is a derived, non-saved mirror.

For coop, the canonical sync field is `AceilingLamp_C::IsActive` (or equivalently `Atrigger_lightRoot_C::IsActive`). There is no enum or brightness float involved in the player-toggle path — it is purely boolean. The `float power` (0x02C4) and `float strengthMult` (0x02D4) are not player-driven; they are design-time constants and power-grid state.

### 3c. Light Actor Discovery — Switch to Lamp Linkage

Linkage is through the `Trigger` reference: `Alightswitch_C::Trigger` (AtriggerBase_C* at 0x02A8) is set in the level's construction. The `Atrigger_lightRoot_C::lights` TArray (0x0298) contains all `AceilingLamp_C*` instances in the group, and `Atrigger_lightRoot_C::ambs` (0x02A8) contains `AambientLight_C*` instances.

Discovery at coop hook time: the switch has a `Key` (FName, inherited from triggerBase at 0x0260) for save-system identification. This same `Key` can serve as the wire identifier for the switch, since it is already used by the save system for consistent cross-load identification.

For coop sync we do not need to enumerate individual lamps: hook `Atrigger_lightRoot_C::SetActive(bool)` POST on the sender (it fires for any source — switch, power board, script event). Invoke `Atrigger_lightRoot_C::SetActive(bool)` on the receiver with the same boolean. All lamps in the group are updated atomically as in single-player.

### 3d. Save Persistence

`AceilingLamp_C` has `getTriggerData`/`loadTriggerData` — lamp `IsActive` is save-persistent. `Atrigger_lightRoot_C` also has `getTriggerData`/`loadTriggerData` — the root's `IsActive` is saved too.

`Alightswitch_C` does NOT override `getTriggerData`/`loadTriggerData` (the method list at lightswitch.hpp shows only `gatherDataFromKeyT` but not the data pair). The switch's `bool A` is NOT independently saved. On load, the game restores lamp state from the lamp's own save record, not from the switch.

For Phase 5S0 (snapshot-on-connect): light state is covered by the host save — lamps are restored from saved `IsActive`. No additional light-specific snapshot needed beyond what Phase 5S0 already provides.

---

## Section 4 — Existing In-Scene Quantities

**Doors**: The `Adoor_C` class is referenced across many building assembly actors as `UChildActorComponent* door` entries: `breakroomCounter.hpp` (5 doors), `bunkhouse.hpp` (1 door), `dish.hpp` (1 door), `cremator.hpp` (1 door). The base building (`baseBuilding.hpp`) has fire doors in the base structure. `soltomiaCleaning.hpp` references 4 doors (1 primary + 3 jammed). A rough estimate from the headers: **15-30 `Adoor_C` instances** in a standard map session, depending on story progress and opened/spawned events. These include: base doors, bunker/mine doors, outbuilding doors, cargo lift doors (2x). All are pre-placed level actors (not runtime-spawned), so their Key FNames are stable across sessions.

`ApowerControl_C::doorsOpen` TArray suggests at minimum 1-3 doors controlled by the power board.

**Light switches / lamp groups**: `mainGamemode.hpp` line 257 has `TArray<AceilingLamp_C*> allCeilingLights` (0x0DB8). This is the game's own tracking array, populated at BeginPlay. A rough estimate: **20-50 `AceilingLamp_C` instances** (base has multiple rooms, each with several fixtures), grouped into **5-15 `Atrigger_lightRoot_C`** groups (one per room/zone), driven by **5-15 `Alightswitch_C`** switches.

The `Atrigger_lightRoot_C::SetActive` path updates all lamps in a group atomically. For coop, we sync per-group (one packet per switch event) rather than per-lamp. At 5-15 groups, bandwidth is negligible.

---

## Section 5 — Proposed Wire Protocol

### 5a. Door State Packet — `DoorStatePayload`

Target: reliable (state must not be lost), ≤ 32 bytes payload.

Fields:

```cpp
struct DoorStatePayload {
    uint8_t triggerKeyLen;      // 1 — length of door's FName Key string
    char    triggerKey[23];     // 23 — door's AtriggerBase_C::Key (FName string)
    uint8_t action;             // 1 — 0=close, 1=open, 2=jam, 3=unjam
    uint8_t bypassCheck;        // 1 — 1 = skip lock validation on receiver (always 1 for coop)
    uint8_t _pad[5];            // 5 — padding to 32 bytes
};
// Total: 32 bytes
static_assert(sizeof(DoorStatePayload) == 32);
```

The `triggerKey` is the door actor's inherited `AtriggerBase_C::Key` FName (0x0260). This is a save-assigned FName assigned during `intComs_gamemodeMakeKeys` (seen in triggerBase.hpp:120). It is stable across sessions (set at map load from the save/level data, not randomized like Aprop_C keys). Resolution on receiver: `GetKey()` UFunction query on all `Adoor_C` instances in scene (or cached map at session connect-time).

The `action` field:
- `0` = `doorClose(bypassCheck=true)` — receiver calls close
- `1` = `doorOpen(bypassCheck=true)` — receiver calls open
- `2` = `jam(autoCancel=false)` — receiver calls jam
- `3` = `jamCancel(duration=0)` — receiver cancels jam

ReliableKind assignment: `DoorState = 9` (next after `TeleportClient = 8`).

### 5b. Light Switch State Packet — `LightStatePayload`

Target: reliable (state must not be lost), ≤ 32 bytes payload.

```cpp
struct LightStatePayload {
    uint8_t triggerKeyLen;      // 1 — length of switch's FName Key string
    char    triggerKey[23];     // 23 — Alightswitch_C::Key OR Atrigger_lightRoot_C::Key
    uint8_t isOn;               // 1 — 0=off, 1=on (maps to SetActive argument)
    uint8_t _pad[7];            // 7 — padding to 32 bytes
};
// Total: 32 bytes
static_assert(sizeof(LightStatePayload) == 32);
```

Design decision: use `Atrigger_lightRoot_C::Key` as the identifier, not the switch's Key. The root is the authoritative group controller and is what the save system uses to persist lamp state. The switch's `bool A` is a display mirror. Hooking `Atrigger_lightRoot_C::SetActive(bool)` POST on the sender captures all sources (player switch, power board, script event). The receiver finds the lightRoot by Key and calls `SetActive(bool)`.

ReliableKind: `LightState = 10`.

### 5c. Password Lock State Packet (door-adjacent) — `LockStatePayload`

A player interacting with a password lock or keycard reader creates a lock-state change that opens a door. This is a separate concern from the door itself.

```cpp
struct LockStatePayload {
    uint8_t triggerKeyLen;      // 1 — ApasswordLock_C::Key
    char    triggerKey[23];     // 23
    uint8_t isAccepted;         // 1 — 1=accepted/unlocked, 0=reset/locked
    uint8_t _pad[7];            // 7
};
// Total: 32 bytes
```

The receiver calls `ApasswordLock_C::Open(bool Active)` (line 78 in passwordLock.hpp) with the `isAccepted` value. `Open()` triggers the door internally via the stored `door` pointer.

ReliableKind: `LockState = 11`.

### 5d. Reliability

All three packets ride the existing `ReliableChannel` (stop-and-wait ARQ, 250 ms RTO). Door and light toggles are low-frequency (< 1/second in normal play). The stop-and-wait design means at most one door/light event is in-flight at a time per direction — perfectly adequate for this traffic pattern.

Size check: All three payloads are 32 bytes. `32 < kMaxReliablePayload (228)` — fits in one datagram with ample headroom.

### 5e. NPC-Triggered Door Events

NPC auto-open via sensor overlap MUST be broadcast. The NPC runs on both peers independently (host-authoritative per [[project-coop-enemies-target-both]]). If the NPC opens a door on the host, the client must see the door open for visual consistency. The hook on `doorOpen(bool bypassCheck)` POST catches NPC-triggered opens identically to player-triggered opens — no special NPC path needed. The `by` field is omitted from the wire payload (the receiver doesn't need to know who triggered it).

---

## Section 6 — Host-Authoritative or Symmetric?

### Recommendation: Host-Authoritative for NPC-triggered; Symmetric for Player-triggered

Rationale:

**Door open/close by player**: Both peers can trigger a door (either peer can walk up and press E). The door state is simple boolean (`isOpened`). Unlike props (which have world-space Key UUIDs and spawn ordering issues), doors are pre-placed actors with stable Key FNames. Either peer can send a `DoorState` packet when THEY open/close a door locally. The receiver applies it immediately. This is symmetric, similar to pose updates.

Risk of symmetric: if both players simultaneously press E on the same door (microseconds apart), both send `DoorState` open packets. The receiver gets two `doorOpen` calls in rapid succession. `doorOpen` is idempotent when `isOpened` is already true (the BP should guard: "if already open, return"). This needs confirmation (RE Flag E-D4) but is the expected behavior for a well-written BP.

**Door open/close by NPC**: NPC AI runs only on the host (per [[project-coop-enemies-target-both]]). The client's mirror NPC does not pathfind autonomously — it follows driven poses. The door sensor overlap for the NPC will only fire on the host. Host sends `DoorState` packet; client applies. This is host-authoritative for NPC-triggered opens.

In practice, the single hook on `doorOpen`/`doorClose` POST covers both cases from the sender's perspective. The sender (host OR client) sends the packet when they locally execute a door state change. The receiver applies it. This is the same pattern as prop grab (PropPose from the holding peer) — symmetric sender, receiver follows.

**Light switch**: Symmetric. Either peer can flip a switch. The lightRoot `SetActive` fires on the triggering peer; they send `LightState`; receiver calls `SetActive` on their lightRoot. Low collision risk (two players simultaneously flipping the SAME switch is rare and the double-SetActive is idempotent).

**Password lock**: Host-authoritative is SAFER here because the password-entry UI is complex (digit-by-digit entry, the `isFocused` state, the `entering` bool). Two peers entering passwords simultaneously on the same keypad would be bizarre UX. For phase 1, gate keypad interaction: only the host's E-press on a lock triggers the `LockState` packet. The client sees the lock accept/reject visually but cannot initiate a lock interaction themselves yet (future UX decision). This avoids complex input-merge issues.

**Summary**:

| Actor | Authority |
|---|---|
| `Adoor_C` open/close by player | Symmetric (sender sends on own E-press or own sensor) |
| `Adoor_C` open/close by NPC | Host-authoritative (only host's sensor fires for NPCs) |
| `Alightswitch_C` / `Atrigger_lightRoot_C` | Symmetric |
| `ApasswordLock_C` | Host-authoritative (client observes only) |

---

## Section 7 — Increment Plan

### Inc1: Door Key Resolution Infrastructure

**Scope**: At session connect, enumerate all `Adoor_C` actors in the scene and build a `FName → Adoor_C*` lookup map. Use `AtriggerBase_C::GetKey()` (inherited UFunction at triggerBase.hpp:41) via reflection. Store in `g_doorByKey`. Add `WireTriggerKey` struct (24 bytes: 1 len + 23 data) to `protocol.h`. No packet sent yet.

**Files touched**: `protocol.h`, new `door_sync.cpp/.h` under `coop/`.

**Test**: Log that door Key lookup resolves correctly for all doors in scene.

### Inc2: DoorState Packet — Protocol + Skeleton

**Scope**: Add `ReliableKind::DoorState = 9` to `protocol.h`. Add `DoorStatePayload` struct (32 bytes). Add `ReliableKind::LightState = 10` and `LightStatePayload` struct (32 bytes) for completeness. Add static_asserts. No observers yet.

**Files touched**: `protocol.h`.

**Test**: Compile passes; size asserts pass.

### Inc3: Host-Side Door Observer (sender)

**Scope**: Hook `Adoor_C::doorOpen(bool bypassCheck)` POST and `Adoor_C::doorClose(bool bypassCheck)` POST. On fire: read the door actor's `Key` (offset 0x0260 from `AtriggerBase_C`), serialize `DoorStatePayload`, send via `ReliableChannel::Send(ReliableKind::DoorState, ...)`. Gate: if this is an incoming-apply (echo-suppression set), skip. Works on host AND client (symmetric sender).

**Files touched**: `door_sync.cpp`.

**Test**: Autonomous LAN test: host opens a door; log shows packet sent + key logged.

### Inc4: Receiver-Side Door Apply

**Scope**: In session's `TryDrain` loop for `ReliableKind::DoorState`: parse `DoorStatePayload`, look up actor via `g_doorByKey[key]`, call `doorOpen(bypassCheck=true)` or `doorClose(bypassCheck=true)` via `coop::call::CallUFunction`. Add echo-suppression: mark door as "incoming-apply in progress" before the call, clear after.

**Files touched**: `door_sync.cpp`, session drain handler.

**Test**: Autonomous LAN test: host opens a door; client's door opens. Log both sides.

### Inc5: Light Switch Observer + Apply (sender + receiver)

**Scope**: Hook `Atrigger_lightRoot_C::SetActive(bool Active)` POST. On fire: read the root's `Key` (0x0260), serialize `LightStatePayload` with `isOn=Active`, send via ReliableChannel. At session connect, build `g_lightRootByKey` (same enumeration pattern as doors). Receiver drain: find lightRoot by Key, call `SetActive(bool)` via reflection.

**Files touched**: `door_sync.cpp` (or split to `light_sync.cpp` if approaching soft cap).

**Test**: Autonomous LAN test: host flips a switch; client's lights toggle. Log both sides.

### Inc6: Password Lock State (host-authoritative)

**Scope**: Add `ReliableKind::LockState = 11`, `LockStatePayload` (32 bytes). Hook `ApasswordLock_C::Open(bool Active)` POST on the host only (role guard). Send `LockStatePayload`. Receiver: find lock by Key, call `Open(bool)`. Build `g_lockByKey` at connect.

**Files touched**: `protocol.h`, `door_sync.cpp`.

**Test**: Host authenticates a password lock; client sees door open.

### Inc7: Snapshot-on-Connect Door/Light State Restoration

**Scope**: On client connect (Phase 5S0 host-side), enumerate all doors and lightRoots. For each door: read `isOpened` (0x0350); if `isOpened`, send `DoorState{action=open, bypassCheck=true}`. For each lightRoot: read `IsActive` (0x02B8); send `LightState{isOn=Active}`. These ride the reliable channel (queued after PropSpawn batch). Use `settime(bool opened)` (door.hpp:120) on the receiver to snap without animation (instant state restore at connect time).

**Files touched**: `prop_snapshot.cpp` (or `door_sync.cpp`) — whichever handles the connect snapshot.

**Test**: Client joins mid-session; all open doors and lit rooms match host state immediately.

---

## Section 8 — Open RE Flags

**E-D1**: `Adoor_C::alienated` (bool 0x03B1) — unknown semantics. What sets it? What door behavior does it modify? Needs UE4SS Lua probe: observe field before/after an alien-related story event, and check if it gates `player_use` or `doorOpen`.

**E-D2**: `Adoor_C::player_use` BP body — the exact BP graph is unconfirmed. Specifically: does it call `doorOpen` if closed and `doorClose` if open (toggle), OR does it only open (requiring a separate close mechanism)? Needs UE4SS Lua probe: hook `player_use` PRE, read `isOpened`; hook `doorOpen`/`doorClose` POST; confirm which fires on a single E-press when door is open vs closed.

**E-D3**: `Adoor_C::checkSensor()` BP body — the exact NPC filtering logic. Does it check `OtherActor->IsA(ACharacter::StaticClass())`? Or does it filter by tag? Or does it include everything (players + NPCs + props rolling into it)? If props can trigger sensor auto-open, the coop sender-side hook would generate spurious packets for physics events.

**E-D4**: `Adoor_C::doorOpen()` idempotency when `isOpened` is already true — does the BP body early-return, or does it restart the timeline? If it restarts, simultaneous E-press by both peers causes a double-animation jitter on the receiver. Needs UE4SS probe: call `doorOpen` twice rapidly; observe animation state.

**E-D5**: `Adoor_C::move__UpdateFunc()` internals — how does the timeline alpha map to panel positions? Is it a simple `SetRelativeLocation` lerp using `lerpA_L`/`lerpA_R` → `openDoor`, or does it involve SetRelativeRotation (swing) vs SetRelativeLocation (slide)? This affects whether there is a "direction" the door swings toward based on approach angle, which could look wrong on the receiver if the panels end up in a different position. Likely confirmed by examining the `ramp1` component (0x0298) — the presence of a ramp suggests the door slides up (like a garage door) rather than swings.

**E-L1**: `Alightswitch_C::player_use` BP body — does it call `use()` directly, or does it go through `actionOptionIndex`? Also: does `use()` fire `Trigger` via `fireTrigger(this, 0)` or via `runTrigger(this, 0)` or a direct function call on the Trigger actor? This determines the exact hook target for the light observer.

**E-L2**: `Atrigger_lightRoot_C` `IsActive` vs `Active` duality — two bool fields at 0x02B8 and 0x02B9. Is `Active` the "desired" value and `IsActive` the "applied" value (deferred-update pattern)? Or are they the same state with one being a transient flag? The `buffIsActive` (0x02E0) further complicates this. Needs UE4SS probe: toggle a switch, snapshot both fields before/after `SetActive` call.

**E-D6**: Door Key FName stability across sessions — `AtriggerBase_C::Key` for doors is set during `intComs_gamemodeMakeKeys` (triggerBase.hpp:120). Unlike `Aprop_C::Key` which uses `NewGuid` (random UUID), trigger keys are presumably deterministic from the level placement. However, the exact key assignment mechanism (sequential integer vs level-specific name) is unconfirmed. If keys are sequential and doors can be dynamically created/destroyed, the key assignment order could differ between peers. Needs: UE4SS Lua probe to dump all door Keys at the same session point on two instances and compare.
