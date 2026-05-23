# VOTV physics-prop pickup/drag surface — static analysis

Date: 2026-05-23
Source: `Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/`
Scope: enumerate the full state surface across `Aprop_C` and `AmainPlayer_C` for
the physics-prop pickup/drag interaction system, in support of coop replication.
Constraint: static analysis only; no probes, no code changes.

---

## TL;DR — what crosses the wire (one paragraph)

Every grabbable thing in VOTV derives from `Aprop_C`. The "is this heavy?" gate
is a **data-table-driven boolean** (`Fstruct_prop.heavy` at offset 0x6C in the
prop's `propData`), not a runtime mass comparison. The host-authoritative
minimum-viable sync is: `(actor_id, component_name, grab_mode {light|heavy},
grab_anchor_local_offset, drop_event, throw_impulse)` plus a streamed
**world-space pose** of the held prop at the rate of pose updates. On the
puppet side we mirror by **disabling physics on the satellite prop** and just
SetActorLocationAndRotation each tick to the streamed transform — the source
already drives the rigid body via `UPhysicsHandleComponent` /
`UPhysicsConstraintComponent`, so its world position is the truth. The
"animation" of the source player's holding arm is already handled by the
existing AnimBP path on the puppet (the satellite character / kerfur AnimBP);
we only need to pose the prop and toggle some prop-side cosmetic flags.

---

## 1. The prop base class — `Aprop_C` (`prop.hpp`)

Header dump: `prop.hpp` lines 4-200 (size: 0x363).
Inheritance: `Aprop_C : public AActor`.

**456 classes derive directly from `Aprop_C`** (counted by grep over the dump).
~85 more derive transitively (e.g. from `Aprop_box_C`, `Aprop_food_C`,
`Aprop_container_C`, `Aprop_gascan_C`, `Aprop_toolbox_C`). Total grab-surface
classes: **~540**.

`Aprop_C` is also used through Blueprint interfaces — `BPI_main` is implemented
by basically every interactable actor (the BPI functions appear on 60+
non-`Aprop_C` actors: ATV, doors, woodchipper, etc., which can also be
heavy-dragged via the same code path).

### 1.1 Fields on `Aprop_C` that matter for grab/drag

| Field | Offset | Type | Purpose |
| --- | --- | --- | --- |
| `propThrown` | 0x0228 | `UpropThrown_C*` | Helper actor-component that tracks a thrown prop and detects ground/wall hits |
| `physicsImpact` | 0x0230 | `Ucomp_physicsImpact_C*` | Holds **`Mass`** (0x01F0), **`ImpactThreshold`** (0x01F4), `boundsLocationRotation()`. The "real" physical handle. |
| `StaticMesh` | 0x0238 | `UStaticMeshComponent*` | The body that simulates physics — this is what `UPhysicsHandleComponent::GrabComponentAtLocation` is invoked on |
| `propPicker` | 0x0240 | `FDataTableRowHandle` | Lookup key into the master prop DataTable (holds the `Fstruct_prop` row) |
| `Name` | 0x0258 | `FName` | The DataTable row name (e.g. `"prop_gascan_big"`) |
| `propData` | 0x0260 | `Fstruct_prop` (0x78) | **The static descriptor** — see §1.2 |
| `Static` | 0x02D8 | `bool` | Locked in place; cannot be grabbed |
| `frozen` | 0x02DA | `bool` | Physics disabled until something wakes it |
| `sleep` | 0x02DD | `bool` | Currently sleeping (wakes on touch/awoken delegate) |
| `Key` | 0x02E0 | `FName` | Persistent save key — unique per prop instance |
| `flameBase` | 0x0308 | `Ucomp_flammable_C*` | Optional fire state (irrelevant here, but state we already replicate elsewhere may overlap) |
| `collisionType_light` | 0x0360 | `ECollisionChannel` | Collision channel while in light/grab mode |
| `collisionType_heavy` | 0x0361 | `ECollisionChannel` | Collision channel while in heavy/drag mode |

**Delegates (multicast) on the prop:**
- `Hit` (0x02E8), `unhooked` (0x02F8), `takenByPlayer(prop)` (0x0328),
  `awoken` (0x0340), `touched` (0x0350).

**Key UFunctions (BPI surface) that mainPlayer calls into:**
- `playerGrabbed_pre(Player, HitResult)` — fired before the grab is committed
- `playerGrabbed(Player, HitResult)` — fired after grab is committed
- `playerTryToGrab(Player, &collected)` — gate: lets the prop refuse the grab
- `playerTryToHold(Player, &collected)` — gate for the inventory "hold" path
- `playerTryToCollect(Player, &collected)` — gate for collect-to-inventory
- `playerHoldPre(Player)` / `playerHoldPost(Player)` — fired around inv-hold change
- `playerUnequip(Player)` / `unequpped(Player)` — fired when player switches off this prop
- `playerHitWith(Hit, Player)` — fired when this prop hits something while held
- `thrown(Player)` — fired when the player throws this prop
- `kicked(bool kick)` — fired when this prop is kicked
- `damageByPlayer(Player, Hit, Damage)` — fired when the player damages it
- `beginHoldingObject(Player, Hit)` — fired at start of a hold cycle
- `playerUsedOn(Player, Hit, lookAtComp, holdObject, holdPropName)` — fired when
  the *player's* held item is used on this prop (e.g. battery into generator)
- `switchToHeavyDrag(&isHeavy)` — gate: prop returns whether it must be heavy-dragged
- `canPickup(&return)`, `canBePickedUp(&ignore)`, `canBeCollected(&ignore)` — gates
- `canHit(&noHit)`, `canBeUsedHold(&return)`, `canBePutInContainer(&return)`,
  `noRespawn(...)`, `gatherDataFromKey(...)`, `getData(&Data)`, `loadData(Data, &return)`
- `setPropProps(Static, frozen, Active, sleeping)` — sets the four flags
- `setKey(Key)`, `setIgnoreSave(ignore)`

### 1.2 `Fstruct_prop` — the data-table row that drives grab/drag

File: `struct_prop.hpp`. Used as `Aprop_C.propData`.

```cpp
struct Fstruct_prop {
    UStaticMesh* mesh;            // 0x00
    FText displayName;            // 0x08
    FText description;            // 0x20
    TSubclassOf<AActor> spawnAsObject; // 0x38
    bool parseNameToObject;       // 0x40
    enum_spawnmenuTabs category;  // 0x41
    int32 price;                  // 0x44
    float massMultiply;           // 0x48  <- multiplier on physical mass
    FName achievement_unlock;     // 0x4C
    FString craftTag;             // 0x58
    float volumeMultiply;         // 0x68  <- multiplier on physical volume
    bool  heavy;                  // 0x6C  <- IS THIS A "HEAVY/DRAG" PROP?
    bool  canHold;                // 0x6D
    bool  canCollect;             // 0x6E
    bool  ignoreInteractions;     // 0x6F
    bool  hidden;                 // 0x70
    bool  spoiler;                // 0x71
};                                // sizeof = 0x72
```

This is the **only** static "heavy" classification in the game data. The
threshold for "heavy" is therefore **not** a mass comparison at grab time — it
is **pre-baked** as a bool in the DataTable row for each prop kind.

Implication for coop: the host can announce `(prop_name, grab_mode)` and the
client can derive the same `grab_mode` from its own DataTable lookup of
`Name` → `propData.heavy`. **The classification does not need to cross the
wire**, only the binary "this prop is being grabbed by player X now".

### 1.3 `Ucomp_physicsImpact_C` — the runtime physical state component

File: `comp_physicsImpact.hpp`. Attached to every `Aprop_C`.

Important fields:
| Field | Offset | Type |
| --- | --- | --- |
| `Owner` | 0x00B8 | `AActor*` |
| `HitComponent` | 0x00C0 | `UPrimitiveComponent*` (the simulating mesh) |
| `Mass` | 0x01F0 | `float` (the **actual physical mass**, cached at Init) |
| `ImpactThreshold` | 0x01F4 | `float` |
| `Damage` | 0x0200 | `bool` |
| `damageData` | 0x0208 | `Fstruct_breakableProp` (health/dmg/resist) |
| `health` | 0x0238 | `float` |
| `boundsComponent` | 0x0268 | `UPrimitiveComponent*` |
| `physType` | 0x0318 | `int32` |
| `customBreak`, `noSounds`, `disable`, `scrapeDust`, `unbreakable` | various | flags |
| `boundsLocationRotation(&Rot, &Loc, &Extent, &Global)` | UFunction | bounds query |

`Mass` is computed at component Init() from `UPrimitiveComponent::GetMass()`
times `propData.massMultiply`. It is not the gate for heavy/drag — `propData.heavy`
is — but it **is** consumed by the impact-damage and throw-impulse logic.

### 1.4 `UpropThrown_C` — throw tracking

File: `propThrown.hpp`. Attached to every `Aprop_C`.

```cpp
class UpropThrown_C : public UActorComponent {
    UPrimitiveComponent* Component;
    TArray<EObjectTypeQuery> obj;
    FVector lastloc;
    Aprop_C* prop;
    bool canRepeat;
    void Init(Aprop_C* prop, UPrimitiveComponent* Component);
    void repeat();
    void throw();
    void fin();
    void hitted(...);  // bound to HitComponent's OnComponentHit
};
```

When the player throws a prop, `propThrown.throw()` is invoked to start
tracking. The component fires `hitted` on the first ground/wall hit, which
informs `Aprop_C` so it can react (e.g. play sound, damage target).

---

## 2. The player class — `AmainPlayer_C` grab state surface

File: `mainPlayer.hpp` (825 lines, size: 0xF5E). Inheritance:
`AmainPlayer_C : public ACharacter`.

### 2.1 Grab/drag components (the "arm" geometry)

| Field | Offset | Type | Purpose |
| --- | --- | --- | --- |
| `Camera` | 0x0530 | `UCameraComponent*` | The first-person camera (origin of the grab ray) |
| `cameraRoot` | 0x0538 | `UArrowComponent*` | Anchor for camera-relative geometry |
| `lookatBox` | 0x04E0 | `UBoxComponent*` | Look-at trace volume |
| `grabComp` | 0x0658 | `UStaticMeshComponent*` | The **invisible target** that the held prop's UPhysicsHandle is told to chase |
| `grabrot` | 0x0680 | `UArrowComponent*` | The **light-grab anchor pivot** (orientation arrow in front of camera) |
| `grabHandle` | 0x0688 | `UPhysicsHandleComponent*` | The **light grab** mechanism (UE-native physics handle) |
| `heavyRot` | 0x0500 | `UArrowComponent*` | The **heavy-drag anchor pivot** |
| `heavyPull` | 0x05B8 | `UPhysicsConstraintComponent*` | The **heavy drag** primary constraint (pulls the heavy object toward player) |
| `heavyPull_loc` | 0x04E8 | `UPhysicsConstraintComponent*` | A second constraint — likely the "rotational/positional alignment" half |
| `heavyGrab` | 0x04F0 | `UPhysicsConstraintComponent*` | The **heavy grab** constraint (rigid attach point on the prop) |
| `pullComp` | 0x05A8 | `UStaticMeshComponent*` | An invisible chase target for heavy drag (analogous to `grabComp`) |
| `pullSubcomp` | 0x05B0 | `UStaticMeshComponent*` | Secondary heavy-drag chase target |
| `wSubmesh` | 0x05A0 | `UStaticMeshComponent*` | Weapon-arm submesh (held tool/item rendered first-person) |
| `weapon` | 0x0608 | `UStaticMeshComponent*` | First-person held weapon/item mesh |
| `viewmodel` | 0x0610 | `UBillboardComponent*` | Camera-attached anchor for the viewmodel |
| `weaponComp_gravityGun` | 0x05E8 | `Ucomp_gravitygun_C*` | The gravity gun handler — distinct system, see §5 |
| `physicsImpact` | 0x0630 | `Ucomp_physicsImpact_C*` | Player's own physics-impact component |

**Geometry (assembled at runtime via `arm()`, `useArm()`, `setHeavyGrabArm()`,
`Set Heavy Pull Rot()`):**

- `arm(customLength, &Start, &End, &Rotation)` produces the grab raycast.
  `Start` = camera location; `End` = `Start + camForward * customLength`
  (where customLength defaults to a function of `grabLen` and zoom).
- `useArm(&OutHit)` runs the trace against world physics objects.
- For **light grab**, the target point each tick is `grabrot->GetLocation()`
  (the arrow is anchored to the camera/cameraRoot and points forward at
  distance `grabLen`). `grabHandle->SetTargetLocationAndRotation` is called
  every tick in `smoothGrab()` to push the held prop there.
- For **heavy drag**, the prop's center-of-mass is constrained to the player
  via `heavyPull` (a physics constraint), and the *direction* the prop should
  face is set via `Set Heavy Pull Rot()` which writes
  `heavyPull->SetAngularOrientationTarget(...)` (or similar). `heavyGrabArm`
  (0x0E98, FVector) and `heavyGrabLocation` (0x0E8C, FVector) store the
  player-local rest position for the heavy drag, recomputed by
  `setHeavyGrabArm()` based on the prop's bounds.

### 2.2 Grab/drag state fields (the "what am I holding right now?" surface)

| Field | Offset | Type | Meaning |
| --- | --- | --- | --- |
| `grabbing_actor` | 0x07D0 | `AActor*` | **Currently grabbed actor (light)** — null when not grabbing |
| `grabbing_component` | 0x07D8 | `UPrimitiveComponent*` | The simulating component that `grabHandle` is attached to |
| `grabRelativeLocation` | 0x0DE4 | `FVector` | Anchor offset on the prop where the grab was made (component-local) |
| `grabLen` | 0x09D0 | `float` | Distance in front of camera the light grab is held; modulated by mouse-wheel |
| `grabsHeavy` | 0x0874 | `bool` | True while currently heavy-dragging |
| `grabHeavy_pull` | 0x0E20 | `bool` | True during the brief "pulling toward me" transition (heavyObjectPulled state) |
| `Heavy` | 0x0EC8 | `bool` | Current grab mode is heavy (cached) |
| `heavyGrabLocation` | 0x0E8C | `FVector` | Player-local target location for heavy drag |
| `heavyGrabArm` | 0x0E98 | `FVector` | Player-local arm direction for heavy drag |
| `holding_actor` | 0x0A20 | `AActor*` | **Held inventory/use item** (distinct from grab) — the prop the player is "wielding" |
| `holding_name` | 0x0A30 | `FName` | Name of the held item (DataTable key) |
| `holdMesh` | 0x0D18 | `UStaticMesh*` | The mesh assigned to `weapon` while holding |
| `droppedItem` | 0x0A58 | `AActor*` | Most recently dropped actor (for re-pickup avoidance) |
| `lastDroppedItem` | 0x0C50 | `AActor*` | "" |
| `lastDroppedItem_deleted` | 0x0C58 | `bool` | "" |
| `dropped` | 0x0B91 | `bool` | Drop happened this frame |
| `hasDropped` | 0x0DD0 | `bool` | Drop transient |
| `drop_place` | 0x0DD1 | `bool` | Drop was a "place" (precision-placement variant) |
| `drop_dontCollect` | 0x0BB8 | `bool` | Skip auto-collect on next drop |
| `droppedPlaced` | 0x0E48 | `bool` | The drop happened via precision-place path |
| `justDropped` | 0x0CF1 | `bool` | Same-frame drop flag |
| `lookAtActor` | 0x0AA0 | `AActor*` | Actor under crosshair |
| `lookAtComponent` | 0x0840 | `UPrimitiveComponent*` | Primitive under crosshair (used as `grabbing_component` on commit) |
| `LookAtLocation` | 0x0820 | `FVector` | World-space hit point of the crosshair trace |
| `lookatName` | 0x0CA8 | `FText` | UI label of the look-at target |
| `lookAtBoundsReplace` | 0x0ED0 | `UPrimitiveComponent*` | When a prop wants to use a different primitive for its bounds |
| `lookAtState` | 0x0EC9 | `uint8` | Crosshair UI state |
| `lookAtCenter` | 0x0ECA | `bool` | Whether the crosshair is centered |
| `grab_speed` | 0x0C5C | `float` | Linear-stiffness scaling on `grabHandle` (smoothing) |
| `Animation` | 0x0A38 | `bool` | True during a grab-animation transition |

### 2.3 Input/timing state (the "is E held?" surface)

| Field | Offset | Type | Meaning |
| --- | --- | --- | --- |
| `input_use` | 0x0C68 | `bool` | Current state of the `use` input axis (E key) |
| `input_E` | 0x0AD8 | delegate | Multicast for E-state change |
| `input_drop` | 0x0DC0 | `bool` | F key down |
| `holdDelay` | 0x0DE0 | `float` | Time remaining in the "hold E" window before press-becomes-hold |
| `holdDelayInit` | 0x0DDC | `float` | Initial value (length of the press/hold discrimination window) |
| `releaseEToUse` | 0x0E88 | `bool` | After holdDelay expired, release triggers `useSelectedAction` instead of grab |
| `resetScrollOnUseRelease` | 0x0BE0 | `bool` | Scroll-wheel during a use-hold modifies hold target; reset on release |
| `blockScrollWhenUse` | 0x0D9C | `bool` | While use is held, scroll-wheel is captured |
| `hotkeyAction_pressE` | 0x0DD2 | `enum_hotkeyAction::Type` | Configured action for short-press E |
| `hotkeyAction_holdE` | 0x0DD3 | `enum_hotkeyAction::Type` | Configured action for long-hold E |
| `hotkeyAction_swapE` | 0x0DD8 | `bool` | Whether the press/hold actions are swapped |
| `timeUseTimerHandle` | 0x0CF8 | `FTimerHandle` | Timer that fires `timeUseTimer` after `holdDelayInit` |
| `altUse` / `altUseTime` | 0x0CEF / 0x0CF0 | `bool` | Alt-use modifier state |

### 2.4 Grab/drag UFunctions on `AmainPlayer_C`

**State machines:**
- `pickupObject(HitResult)` — Top-level entry, called from input handler.
  Reads `HitResult`, asks the target prop `canPickup` + `switchToHeavyDrag`,
  picks `light` vs `heavy` path.
- `pickupObjectDirect(Actor, Component)` — Bypasses look-at; used for forced grabs.
- `smoothGrab()` — **per-tick** function (called from `ReceiveTick`) that
  drives `grabHandle->SetTargetLocationAndRotation(...)` to keep the prop at
  the camera-relative target. This is the per-tick position writer.
- `setHeavyGrabArm()` — recomputes `heavyGrabArm` based on prop bounds, called
  on grab and on grab-distance changes.
- `Set Heavy Pull Rot()` — writes the heavy-drag orientation target to
  `heavyPull` constraint (called per-tick or on rotation changes).
- `dropGrabObject()` — releases `grabHandle`, breaks `heavyPull` /
  `heavyGrab` / `heavyPull_loc` constraints, nulls `grabbing_actor` /
  `grabbing_component`, fires `objectDropped` delegate.
- `forceDrop()` — same plus dontWakeup/dontPlace flags.
- `throwHoldingProp()` — drops + applies an impulse equal to camForward * f(grabLen).
- `interruptHoldItem()` — drops with `dontWakeup=true, place=false, dontCollect=false`.
- `simulateDrop(dontWakeup, place, dontCollect)` — the actual drop implementation.
- `precisionPlacemet_drop()` — drop variant that places at the precMesh preview transform.
- `destGrabbed(DestroyedActor)` — bound to `grabbing_actor->OnDestroyed`,
  clears state if the prop dies under our hand.

**Gates:**
- `arm(customLength, &Start, &End, &Rotation)` — geometry generator.
- `useArm(&OutHit)` — trace.
- `getUseAction(&use)` — decides press vs hold action.
- `useSelectedAction()` — fires the selected hotkey action.
- `getActionOptions(...)` — populates the radial menu options from the looked-at prop.
- `buildActionList()` — refreshes the radial menu.
- `setActionIndex(actionIndex)` — selects an option.
- `actionIndex` / `max_actionIndex` (0x0A98/0x0A9C, int32) — radial selection cursor.

**Input dispatchers (the 3 `_use_` slots):**

The dispatch numbers in the dump are **assigned in reverse declaration order**
in the BP graph (UE4SS preserves them as-emitted by the BP compiler). The
three `_use_` slots are:
- `InpActEvt_use_K2Node_InputActionEvent_38` (line 562)
- `InpActEvt_use_K2Node_InputActionEvent_41` (line 559)
- `InpActEvt_use_K2Node_InputActionEvent_42` (line 558)

UE's `K2_InputActionEvent` typically generates a Pressed and a Released node
per binding, so two slots are press+release for the primary key, and a third
slot is press for a secondary key bound to the same Action (e.g., a gamepad
button). Without IDA we can't say with certainty which-is-which from the dump
alone — but the **timing path is unambiguous** from the field surface:

1. The user holds E.
2. `input_use = true`; `simulate_usePressed()` is invoked.
3. A timer (`timeUseTimerHandle`) is started for `holdDelayInit` seconds (the
   press/hold discrimination window).
4. If the user releases **before** the timer fires (`releaseEToUse == false`),
   the **short-press** action runs (`hotkeyAction_pressE`).
5. If the timer fires first, `timeUseTimer()` sets `releaseEToUse = true` and
   the **long-press** branch runs (`hotkeyAction_holdE`, typically "grab").
6. On release, `simulate_useReleased()` clears `input_use`, optionally fires
   the hold action if `releaseEToUse` was set.

`pickupObject(HitResult)` is invoked from the long-hold branch. The short-press
branch is dispatched via `hotkeyAction(Selection, Actor)` → routes to
`useAction(false, &succ)` → `useSelectedAction()` for buttons/widgets/doors,
or `Hold Object(useHold, Manual, &collected)` for inventory items.

**Per-frame writer:** `ReceiveTick(DeltaSeconds)` → `smoothGrab()` reads
`grabbing_actor` / `grabbing_component` / `grabrot` / `grabHandle` and pushes
the next target each frame. **This is the hook point we'll intercept** to
read where the source player is moving the held prop.

### 2.5 Heavy-drag delegates (multicast events)

| Delegate | Offset | Fires when |
| --- | --- | --- |
| `heavyObjectGrabbed` | 0x0DF0 | A heavy drag has been established (constraints made) |
| `heavyObjectPulled` | 0x0E00 | The "pull toward me" pre-drag animation completes |
| `heavyObjectDropped` | 0x0E10 | A heavy drag was released |
| `objectDropped` | 0x0C88 | Light grab released |
| `holdObjectChanged_pre` | 0x0DA0 | Inventory hold item is about to swap |
| `holdObjectChanged_post` | 0x0DB0 | Inventory hold item swapped |

These are **the cleanest source-side hook points** for "grab event" and "drop
event" sync. Bind a coop module to all five and the source side emits a
network packet per event.

---

## 3. The "too heavy" threshold — answer

**The threshold is a boolean, not a comparison.**

Path:
1. The player's `pickupObject(HitResult)` examines the target.
2. If the actor implements `BPI_main::switchToHeavyDrag`, the prop returns
   `isHeavy` by reading `propData.heavy` (default behavior on `Aprop_C`).
3. `propData.heavy` is loaded once at `Init()` from the DataTable row at
   `propPicker` keyed by `Name`. It is **never recomputed** at runtime.
4. The player then takes the **light path** (`grabHandle`) or **heavy path**
   (`heavyPull`+`heavyGrab`+`heavyPull_loc` triad) accordingly.

There is **no `maxGrabMass`, no `heavyThreshold`, no `grabMaxWeight`** field
on `AmainPlayer_C`. The mass/volume fields that exist (`physicsImpact.Mass`,
`propData.massMultiply`, `propData.volumeMultiply`) feed throw-impulse,
impact-damage, and physics simulation — not the grab-classification gate.

Some props **also** override `switchToHeavyDrag` explicitly. The dump shows
72 classes declaring it (most are `Aprop_C` derivatives plus the BPI carriers
ATV, doors, etc.). Where overridden, the prop may return a state-dependent
value — e.g., "this barrel is heavy when full, light when empty" — but the
output is still a bool.

**Implication for coop:** the host and client compute the same bool from the
same DataTable (clients have the same `Game_0.9.0n` install). The
classification is therefore **fully deterministic** from `propData.Name`.
We do NOT have to transmit "heavy or light" — but we DO have to transmit
the prop identity (`Name`+`Key` or `actor_id`) and the event ("grabbed",
"dropped", "thrown").

For props that override and produce state-dependent output (e.g. the barrel
example), this is a divergence risk: if the host's barrel is full and the
client's is empty (because some other state failed to sync), the client will
classify wrong and the visible grab will be a mismatch. **Mitigation:**
have the host include `grab_mode` in the grab packet anyway as ground truth
(cheap — one bit).

---

## 4. Grab arm geometry composition

The first-person camera sits at `Camera` (0x0530). Component hierarchy (read
from the order they're declared as `UStaticMeshComponent*` / `UArrowComponent*`
fields):

```
Capsule (inherited from ACharacter)
  └─ cameraLag (USpringArmComponent, 0x04D0)
      └─ cameraRoot (UArrowComponent, 0x0538)
          └─ Camera (UCameraComponent, 0x0530)
              ├─ grabrot (UArrowComponent, 0x0680)
              │   └─ [light grab target, forward by grabLen]
              ├─ heavyRot (UArrowComponent, 0x0500)
              │   └─ [heavy drag pivot]
              └─ viewmodel (UBillboardComponent, 0x0610)
                  └─ weapon (UStaticMeshComponent, 0x0608)
                      └─ wSubmesh (UStaticMeshComponent, 0x05A0)
```

(Note: the actual attachment may differ — UE doesn't expose AttachParent in
the C++ dump. But this is the **logical** composition from naming
conventions and from `arm()`'s output. We can verify with reflection on the
running instance.)

**Light-grab geometry:**
- `grabrot` is the arrow that defines the held position (its world location).
- `grabHandle` is told to track `grabrot->GetComponentLocation()` and
  `grabrot->GetComponentRotation()`.
- `grabComp` is an invisible mesh placeholder that may act as a collision
  proxy or as the visual anchor for the held prop's outline.
- `grabLen` modulates the forward offset: when the player scrolls the mouse
  wheel while holding E, `grabLen` changes (pulled closer / pushed farther).
- The prop's grab anchor (where on the prop the handle attached) is at
  `grabRelativeLocation` (component-local).

**Heavy-drag geometry:**
- `heavyRot` is the pivot arrow.
- `pullComp` and `pullSubcomp` are the invisible "chase target" meshes the
  constraints pin to.
- `heavyPull` (the primary constraint) connects the prop's simulating
  component to `pullComp`, with limited linear stiffness — the prop "pulls"
  toward the player but lags.
- `heavyGrab` connects the prop's simulating component to `pullSubcomp` —
  this likely provides the rotational alignment.
- `heavyPull_loc` is the location-only constraint.
- `setHeavyGrabArm()` computes `heavyGrabArm` (player-local FVector) from
  the prop's bounds (via `physicsImpact.boundsLocationRotation(...)`), so
  larger props are held further from the player to avoid clipping.
- `heavyGrabLocation` is the world-space target each tick.

**For coop puppet mirroring:** the puppet does **not** need to reconstruct
this geometry. The puppet just needs to:
1. Know which prop is held (`actor_id` from the source).
2. Receive the streamed world transform of that prop each tick.
3. Disable physics on the puppet-side copy of that prop (so it doesn't
   gravity-fall while the source has it) and pin to the streamed transform.

If we want the puppet's hands to **point at** the held prop (for visual
fidelity), we feed a virtual "LookAt" target into the puppet's AnimBP using
the prop's world position — same mechanism as the head-look fix in
[[project-remote-player-open-issues]] (zero LookAt_1/LookAt Alphas, write
ModifyBone to spine/hand bones with the computed yaw to the prop). The
existing satellite-ACharacter pattern from [[project-bug2-locomotion-anim]]
already gives us a place to hook this — the satellite's AnimBP is what the
puppet AnimInstance is reading; we can extend it with one or two extra
"IK to held prop" bones.

---

## 5. The gravity gun system — separate, simpler

File: `comp_gravitygun.hpp`, `prop_gravgun.hpp`, `shake_gravGun.hpp`.

`Ucomp_gravitygun_C` is owned by the **player** (`AmainPlayer_C.weaponComp_gravityGun`
at 0x05E8) AND by the **gravity gun prop** (`Aprop_gravgun_C.weaponComp_gravityGun`
implicitly via the gun's actor). Fields:

```cpp
class Ucomp_gravitygun_C : public UActorComponent {
    AActor* obj;                       // held actor
    UPrimitiveComponent* comp;         // held component
    float ad, LD, ang;                 // ang. damping, lin. damping, angular
    float recharge;                    // ammo recharge timer
    Aprop_gravgun_C* gravigun;         // ref back to the prop
    FVector grabDir;                   // pull direction

    void trace(B, &OutHit);
    void hold(obj, &NewParam);
    void ReceiveTick(DeltaSeconds);
    void hookToDestroy();
    void dropped();
    void holdedDestroyed(DestroyedActor);
};
```

`Aprop_gravgun_C : public Aprop_C` (extends the standard prop). Fields:
`obj`, `Origin`, `beeams[]`, `IsActive`, `charges`, `eff_gravigunN/N1/N2`,
`gravigun_object_hold_loop`, `object_throw`, `object_release`, `object_hold_loop`,
`object_grab`, point lights `l1/l2/l3`. UFunctions: `beams()`, `set(obj)`,
`attract(bool)`, `microwaveElec()`, plus tick + a timeline.

The gravity gun uses **the same** `UPhysicsHandleComponent` pattern under the
hood (via `comp_gravitygun_C::hold`), just with different ranges and beam
effects. **It uses the same hook points** as the main hand grab — `obj`,
`comp` are the analogues of `grabbing_actor`, `grabbing_component`.

For coop: gravity-gun-held props are a special case of grabbed props.
We can treat them identically at the wire level — `(actor_id, component_name,
world_transform_each_tick)` — and only the COSMETIC layer (beam effects,
light glow) needs additional sync.

---

## 6. The per-tick held-object position update path — where to intercept

The **single per-tick write** that moves the held prop is inside
`AmainPlayer_C::smoothGrab()`, called from `ReceiveTick(DeltaSeconds)`.
`smoothGrab` reads `grabbing_actor`, `grabbing_component`, `grabrot`,
`grabHandle` and ultimately calls
`grabHandle->SetTargetLocationAndRotation(targetLoc, targetRot)`. The physics
solver then moves the actual `grabbing_component` toward that target.

**Hook strategy (source side):**
- Post-tick, on the source player, read
  `grabbing_actor->GetActorLocation()` /
  `grabbing_actor->GetActorRotation()` and stream them in the pose-update
  packet to peers, alongside `actor_id` (resolved to the persistent `Key`
  field on `Aprop_C`, see §1).
- Or, more precisely, read the world transform of `grabbing_component` (the
  primitive). For most props `grabbing_component == StaticMesh` and its
  transform equals the actor's, but for child components (e.g., the lid of
  a box) it can differ.

**For heavy drag**, the equivalent path is the per-tick movement of
`heavyPull->SetLinearPositionTarget(...)` etc., driven by
`Set Heavy Pull Rot()`. Same intercept — read `grabbing_actor`'s world
transform post-tick.

**Hook strategy (puppet side):**
- Maintain a "held by remote player" set indexed by `actor_id`.
- When the source emits a grab packet: find the local copy of that
  `Aprop_C` (by `Key` field, which is persistent across host/client because
  it's saved/loaded with the world), call `SetSimulatePhysics(false)` on
  its `StaticMesh`, store a pointer in `RemotePlayer.heldProp`.
- Each frame, when applying the streamed pose, also apply the streamed
  prop transform: `prop->SetActorLocationAndRotation(streamed_loc,
  streamed_rot, /*sweep=*/false, /*teleport=*/true)`. Skip sweep+teleport so
  collisions don't fire (we already have the authority on the source).
- On drop packet: `SetSimulatePhysics(true)`, apply the streamed final
  velocity as a one-time impulse (so the throw arc looks right), clear
  `RemotePlayer.heldProp`.

---

## 7. What crosses the wire vs what is derived

### 7.1 Must transmit (host -> all peers)

| Event / field | Reason |
| --- | --- |
| `grab_begin { source_player_id, prop_key, mode {light,heavy}, anchor_local_offset }` | Identifies which prop, who, how. `mode` is technically derivable but cheap and avoids divergence risk (§3). `anchor_local_offset` = `grabRelativeLocation` (where on the prop the grab attached) — needed for visual fidelity of the held position. |
| `grab_pose { source_player_id, prop_world_loc, prop_world_rot }` | Per-tick prop transform, streamed alongside the player pose. Same packet, same cadence (60 Hz). Compressed: 6 floats per held prop. |
| `grab_drop { source_player_id, drop_mode {plain,place,throw}, final_loc, final_rot, final_linvel, final_angvel }` | Drop event with final state. `linvel`/`angvel` needed for throw arcs to look right. |
| `grab_throw { source_player_id, impulse_vector }` | Optional separate packet for throws; or fold into `grab_drop` with `drop_mode=throw` and `impulse=final_linvel-rest`. |
| `prop_damaged { prop_key, source_player_id, damage, hit_loc }` | If we sync damage, fold into the existing damage system (not new). |
| `prop_state { prop_key, broken, fire_on, ... }` | Only for state fields that DON'T re-derive from physics (broken=true, fire=lit). Existing `setPropProps(Static, frozen, Active, sleeping)` is the host-side authoritative setter; mirror it. |

### 7.2 Derive client-side (do not transmit)

| Field | Why derivable |
| --- | --- |
| `propData.heavy`, `canHold`, `canCollect`, `massMultiply`, `volumeMultiply` | Static DataTable; identical on host and client |
| `physicsImpact.Mass`, `ImpactThreshold` | Computed at `Init` from `propData.massMultiply` * mesh-default — identical |
| Light/heavy classification | Derived from `propData.heavy` (with the §3 mitigation) |
| The arm geometry on the source player's PUPPET (visible to other peers) | The puppet doesn't need its own `grabHandle` — it just SetActorLocationAndRotation the prop. The puppet's hand pose can be driven by AnimBP IK to the prop transform (existing pattern). |
| All cosmetic effects (sounds, particles, lights) | Fire identically client-side from the same grab/drop events |

### 7.3 Open question — must transmit OR derive?

| Field | Discussion |
| --- | --- |
| `propThrown` state (currently-flying-after-throw) | Probably derive: receivers see the projectile fly under their local physics, sync via the `grab_pose` stream until rest, then stop the stream. **Risk:** divergence in air. Better: keep streaming until `propThrown.fin()` fires on the source, then stop. One bool ("still streaming") in the packet header. |
| `frozen`, `sleep` flags | Should mirror via `prop_state` — they affect physics simulation determinism |
| `flameBase` (fire) | Already in the existing flammable sync surface; not unique to this feature |
| Inventory-held vs world-grabbed | The `holding_actor`/`holding_name` path (the "I'm wielding a flashlight in 1st person") is **distinct** from `grabbing_actor` (physics grab). For coop, the wielded item is purely cosmetic on the puppet — we can drive it via a simple "currently equipped item name" string in the pose packet, and the puppet's AnimBP / weapon socket renders the corresponding viewmodel. Probably out of scope for THIS feature (physics grab), but worth noting. |

---

## 8. The full list of interactable PROP classes

I won't list all 540. The structural answer is:

- **Direct `Aprop_C` derivatives:** 439 files named `prop_*.hpp`.
- **Direct `Aprop_C` derivatives not named `prop_*`:** 17 files (3dPrinterAnim,
  bed, bed_b, bloodCLot, burningDebris, clocks, customPlank_DUPL_1,
  firetankCorpse, freezerErie_iKnowWhatYouAre, ghostcar, grayboar,
  holeDevice, moabMortar, murderKerfur, relativeField, theEvil, woodchipper).
- **Transitive derivatives (extend an `Aprop_*` subclass):** ~85 files.
  Notable subhierarchies:
  - `Aprop_box_C` (a box container) → puzzle boxes, drive boxes
  - `Aprop_food_C` → ~30 food-item variants
  - `Aprop_container_C` → backpacks, crates, freezers
  - `Aprop_gascan_C` → fuel containers (small/big/funny/full)
  - `Aprop_toolbox_C` → workbench tools

Every one of these participates in the same `Aprop_C` BPI surface. The
**physics-grab feature treats them all identically** — distinguishing them
only by `propData.heavy` and by per-class BPI overrides of
`switchToHeavyDrag` (rare). The coop layer doesn't need a per-class branch.

Non-`Aprop_C` actors that also implement the grab/heavy BPI:
- `AATV_C` (vehicle — heavy drag)
- `Aladder_C`, `AsitBox_C`, `Ahook_C` (NOT grabbable but share the BPI for
  look-at)
- `Adoor_C`, `Acord_C`, `Aanaholk_C`, `Acargolift_C` (interactables, not
  physics-grabbed)

These are listed in the 72-file grep for `switchToHeavyDrag` (§3) but most
return `isHeavy = false` AND fail `canPickup` — they can't be physics-grabbed,
they're just interactable. Coop should still listen for `playerUsedOn` on
these (e.g., opening a door is a coop-replicable event), but that's a
different feature.

---

## 9. Recommended hook points (concrete)

For the eventual implementation in `src/votv-coop/.../coop/physics_grab/`:

### Source-side hooks (host)

1. **UFunction hook on `AmainPlayer_C::pickupObject(HitResult)`** — fires
   AFTER, read `grabbing_actor`, `grabbing_component`, `grabRelativeLocation`,
   `grabsHeavy`/`Heavy`, emit `grab_begin` packet.
2. **UFunction hook on `AmainPlayer_C::dropGrabObject` and `simulateDrop`** —
   fires BEFORE, snapshot `grabbing_actor`'s world transform + linear/angular
   velocity, emit `grab_drop`.
3. **UFunction hook on `AmainPlayer_C::throwHoldingProp`** — extract throw impulse.
4. **Native game-thread tick** — every tick, for each local player with
   `grabbing_actor != null`, snapshot prop world transform and add to the
   outgoing pose packet (extend the existing pose protocol).
5. **Delegate bind on `heavyObjectGrabbed`, `heavyObjectPulled`,
   `heavyObjectDropped`, `objectDropped`** — these are multicast events
   already, just bind a coop receiver. Cleaner than UFunction hooks for the
   heavy path.

### Puppet-side hooks (client, on receipt)

1. **On `grab_begin`:** find prop by `Key` (lookup table maintained by the
   coop module — `Aprop_C.Key` is unique per instance and persistent), call
   `SetSimulatePhysics(false)` on its `StaticMesh`, store in
   `RemotePlayer.heldProp`.
2. **On `grab_pose`:** apply transform via
   `SetActorLocationAndRotation(loc, rot, false, true)`.
3. **On `grab_drop`:** `SetSimulatePhysics(true)`, apply
   `SetPhysicsLinearVelocity(final_linvel)` +
   `SetPhysicsAngularVelocityInDegrees(final_angvel)`, clear
   `RemotePlayer.heldProp`.
4. **Animation sync:** the puppet's IK is driven by the satellite-ACharacter
   AnimBP (existing pattern from [[project-bug2-locomotion-anim]]). Add a
   "held prop world location" field to the satellite, plumbed into the
   AnimBP's existing LookAt nodes (or one new ModifyBone for hand), so the
   puppet's hand visibly tracks the held prop's position.

### Engine layer (`ue_wrap/`)

- `coop::ue_wrap::PropWrap` — wraps `Aprop_C`. Resolves offsets for
  `propThrown`, `physicsImpact`, `StaticMesh`, `propData`, `Key`,
  `collisionType_light`/`heavy`. Exposes `GetKey()`, `GetMass()` (via
  `physicsImpact.Mass`), `IsHeavy()` (via `propData.heavy`), `IsStatic()`,
  `IsFrozen()`. Calls `SetSimulatePhysics`, `SetPhysicsLinearVelocity`,
  `SetPhysicsAngularVelocityInDegrees` on `StaticMesh` via reflection.
- `coop::ue_wrap::PlayerWrap` already exists (extend it) — add resolvers
  for `grabbing_actor`, `grabbing_component`, `grabsHeavy`, `Heavy`,
  `holding_actor`, plus calls to `pickupObject`, `dropGrabObject`,
  `forceDrop`, `throwHoldingProp`, `interruptHoldItem` UFunctions for the
  puppet-replay case (if we choose to drive the puppet via UFunction
  invocation rather than direct SetActorLocationAndRotation).

### Coop layer (`coop/physics_grab/`)

- `coop::physics_grab::Tracker` — tracks the per-player held prop, emits
  `grab_begin`/`grab_drop`/per-tick `grab_pose`. One instance per local
  player.
- `coop::physics_grab::PuppetReceiver` — applies received packets to the
  local prop instance, manages physics on/off.
- `coop::physics_grab::PropKeyResolver` — maintains the `Key` → `Aprop_C*`
  reverse map by scanning at world-load and on actor-spawn delegates.
  (Important: do NOT scan per-frame.)

---

## 10. Risks & open questions

1. **`Key` collisions:** `Aprop_C.Key` is set from the save system. New props
   spawned at runtime (via the spawn menu or by other props' UConstructionScript)
   get keys assigned at `Init`. If host and client both spawn the same prop
   independently (e.g., a freshly-crafted item), they'll get **different**
   `Key`s. **Mitigation:** Coop must route all spawn-from-craft / spawn-from-menu
   through the network layer; the host assigns `Key` and broadcasts. This is
   outside the physics-grab feature but is a prerequisite.

2. **`switchToHeavyDrag` overrides with state-dependent output (§3):** if a
   prop's heavy/light depends on a state field that isn't synced, host and
   client diverge in classification. **Mitigation:** include `mode` in the
   `grab_begin` packet anyway. Cheap, robust.

3. **`grabbing_component != StaticMesh`:** some props have sub-components
   that can be grabbed independently (a box lid, an ATV wheel). Naming the
   sub-component over the wire requires either the **component name** (string)
   or an enumerated index. `UPrimitiveComponent::GetFName()` gives us a stable
   identifier; transmit it as `FName` (UE FName is shared across host/client
   for cooked content).

4. **Constraint break under unusual conditions:** if a heavy-dragged prop
   collides with a wall hard enough, `heavyPull->BreakConstraint()` may fire
   (UE constraints have configurable break thresholds). The source-side
   delegate `heavyObjectDropped` covers this — bind it.

5. **`propThrown` post-throw tracking:** after a throw, the source's
   `propThrown` component watches for the first ground/wall hit. If we stop
   streaming `grab_pose` immediately on `grab_drop`, the puppet's prop will
   fall under its local physics — usually fine for short flights, but for
   long-range throws (e.g., off a cliff) the trajectories will diverge.
   **Mitigation:** keep streaming `grab_pose` until `propThrown.fin()` fires
   on the source. Add a "post-throw streaming" mode to the protocol.

6. **The arm geometry (§4) is reconstructed from naming, not from
   AttachParent.** Verify with reflection on a live instance before relying
   on the exact composition. The hook points listed above don't actually
   depend on this (we drive the puppet by direct transform, not by
   reconstructing the arm), but the in-game "where does the prop visually
   appear in the puppet's hand" depends on it for the AnimBP IK approach.

7. **`UpropThrown_C.hitted` is bound to `HitComponent->OnComponentHit`:** if
   the puppet's local copy is in `SetSimulatePhysics(false)` mode, this hit
   delegate won't fire on the puppet. That's correct — we don't want the
   puppet to double-fire impact effects — but be aware that any prop-side
   logic that watches its own hits will go quiet on the puppet. The
   source-side authoritative emit covers this (sound/damage messages cross
   the wire).

---

## Appendix A — file references

- `prop.hpp` — `Aprop_C`, lines 4-200, size 0x363
- `struct_prop.hpp` — `Fstruct_prop`, the heavy/canHold/canCollect data
- `struct_propDynamic.hpp` — `{name, key}` runtime instance descriptor
- `struct_breakableProp.hpp` — `{health, damage, impactResistance, gibs}`
- `mainPlayer.hpp` — `AmainPlayer_C`, lines 1-823, size 0xF5E
- `comp_physicsImpact.hpp` — `Ucomp_physicsImpact_C`, has `Mass` (0x01F0)
- `comp_gravitygun.hpp` — `Ucomp_gravitygun_C`, gravity gun handler
- `prop_gravgun.hpp` — `Aprop_gravgun_C : Aprop_C`, gravity gun prop
- `propThrown.hpp` — `UpropThrown_C`, post-throw tracker
- `Engine.hpp` — `UPhysicsHandleComponent`, `UPhysicsConstraintComponent`,
  `UPrimitiveComponent::GetMass()`/`SetSimulatePhysics()`/`SetPhysicsLinearVelocity()`
- `enum_interactionActions_enums.hpp` — names stripped (NewEnumerator0..14)
- `enum_hotkeyAction_enums.hpp` — names stripped (NewEnumerator0..9)

---

## Appendix B — quick stats

- Total `Aprop_C` direct derivatives: 456 (439 named `prop_*`, 17 others)
- Total props transitively grabbable: ~540
- Classes overriding `switchToHeavyDrag`: 72 (most just inherit the BPI; only
  a handful return state-dependent values)
- Mass threshold field: **none** (gate is data-driven bool `propData.heavy`)
- Per-tick grab writer: `AmainPlayer_C::smoothGrab()` (via `ReceiveTick`)
- Key state to wire-sync: 6 floats (loc+rot) per held prop per tick, plus
  begin/drop/throw events with ~30 bytes each
