# RE: VOTV inventory→world drop spawn pipeline (Bug C)

**Date**: 2026-05-24
**Bug**: Bug C — when peer A drops an item from inventory, peer B sees nothing.
**Stage-4 wire only handles props that exist on BOTH peers (Key-resolved).
Inventory drops create fresh actors with fresh Keys peer B has never indexed.**

**Constraint** (per [[project-coop-inventory-private]]): inventories are
per-peer private. Only the world drop EVENT crosses the wire — never the
inventory contents.

---

## Section 0 — Method note: why this is reflection-driven, not IDA-disasm

The shipping `VotV-Win64-Shipping.exe` interprets Blueprint bytecode at
runtime through `UObject::ProcessEvent`. BP functions like
`simulateDrop`, `getObject`, `takeObj`, `extract` ARE NOT compiled to
native x64; they are sequences of UE FFrame opcodes stored in cooked
`.uasset` content, dispatched serially by the engine VM. IDA can show
the engine VM (`UObject::ProcessEvent` + interpreter loop), but it
CANNOT pseudo-decompile the BP body of any of these functions — they
have no native function address.

Three consequences:

1. The **SDK header signature** (from UE4SS CXXHeaderDump) is the
   authoritative description of param order + types — derived from the
   cooked `.uasset` metadata, not from IDA. Every UFunction declared
   below has its full signature; what we DO NOT have without runtime
   probing is the body's exact callee sequence.
2. **Empirical confirmation** runs through the existing ProcessEvent
   observer infrastructure (the same one that proved Stage-4 PHC + PCC
   hooks): hook ProcessEvent, filter for the listed UFunction names,
   log the args at the moment of dispatch. This is the right tool for
   the body-level callee chain.
3. **IDA contributes** the native-side anchors: `UWorld::SpawnActor`
   (where the BP `SpawnActorFromClass` node eventually lands), the
   `BeginDeferredActorSpawnFromClass` / `FinishSpawningActor` exec
   thunks (FName entries set as comments in the IDB at 0x144199b88 /
   0x144199de0 / 0x1441ac378 / 0x1441abea8), and the existing
   reflection thunks already named by prior agent sessions.

Order followed per `CLAUDE.md` "escalation ladder": reflection (SDK
header + auto-memory of confirmed offsets) is first; IDA is for the
native-side anchors only; UE4SS Lua probes will be the third step IF
the empirical PE-hook observer leaves a callee ambiguous.

**Bottom line**: the SDK headers below are not guesses. They are the
authoritative wire schema. The body-level callee chain WILL be
verified by PE observer once Stage 5 implementation lands.

---

## Section 1 — Inventory drop call graph

```
USER PRESSES Q (drop input)
  |
  v
AmainPlayer_C::InpActEvt_drop_K2Node_InputActionEvent_0(FKey Key)
AmainPlayer_C::InpActEvt_drop_K2Node_InputActionEvent_1(FKey Key)
  |
  | (BP graph: branches on tap vs hold; tap = quick drop, hold = precision place)
  |
  v
AmainPlayer_C::simulateDrop(bool dontWakeup, bool place, bool dontCollect)
  |  --- the routing core; reads mainPlayer state to decide which path:
  |
  |--(A) HOLDING a physics-grabbed prop (mainPlayer.grabbing_actor != null)
  |     |
  |     v
  |   AmainPlayer_C::dropGrabObject()
  |     -- releases the PhysicsHandle/PhysicsConstraint; the prop is
  |        already a world actor on both peers; NO new SpawnActor.
  |     -- Stage-4 wire already covers this path (PropRelease packet).
  |
  |--(B) HOLDING an inventory ITEM with no world body
  |     (mainPlayer.holding_actor != null  -- this is the inventory hold-in-hand,
  |      a transient AActor* the BP graph spawned earlier to preview the item)
  |     |
  |     v
  |   AmainPlayer_C::objectDropped()           [delegate fires]
  |     -- the held actor IS already a world actor in the local viewport;
  |        we just unparent it from the player and re-physics it.
  |     -- However the OTHER peer never saw it spawn (it spawned local-only
  |        when item was equipped). This IS one of the Bug-C paths.
  |
  |--(C) EQUIPMENT slot (e.g. flashlight tagged in mainPlayer.equipment[])
  |     |
  |     v
  |   AmainGamemode_C::RemoveEquipment(int32 Index, bool Drop, bool& return)
  |     -- if Drop=true, spawns a world Aprop_equipment_*_C actor at the
  |        player's drop transform with the equipment's saved Fstruct_save.
  |     -- This IS the second Bug-C path.
  |
  |--(D) OPEN container UI (player has a container UI open, e.g. backpack)
        |
        v
      Aprop_inventoryContainer_player_C::extract(int32 Index)
        |  -- override of Aprop_container_C::extract
        |
        v
      Aprop_container_C::extract(int32 Index)
        |  -- calls into the contained UpropInventory_C
        |
        v
      UpropInventory_C::takeObj(int32 Index, bool removeVol, bool& return,
                                 Fstruct_save& Output, AActor*& Object)
        |  -- removes the item from this inventory's array;
        |  -- spawns the world actor:
        |       UWorld::SpawnActor(Output.class_3_*)  via K2Node_SpawnActorFromClass
        |       --> calls UGameplayStatics::BeginDeferredActorSpawnFromClass
        |       --> sets transform = Output.transform_8_*
        |       --> calls UGameplayStatics::FinishSpawningActor
        |  -- calls Aprop_C::loadData(Output) on the spawned actor to restore
        |     Key + state (bools/floats/ints/etc.).
        |
        v
      The new world actor is returned in `Object`. From this point it is
      a normal Aprop_C-derived world actor with a stable Key; Stage-4
      PropPose tracking would work IF the receiving peer had spawned
      the same actor with the same Key — which IS the missing piece.
```

Helper that bypasses the inventory entirely (e.g. cheat-menu spawn):
```
AmainGamemode_C::spawnPropThroughGamemode(FName prop, FTransform InputPin,
                                          int32 Amount, AActor*& actor)
```
This one we DO NOT need to wire — its callers are debug / cheat / world
gen paths, not coop-replicated user actions.

---

## Section 2 — SpawnActor invocation details

The BP `SpawnActorFromClass` node compiles to a two-step deferred spawn
(allowing the BP to set instance properties between `Spawn` and `Init`):

```
UGameplayStatics::BeginDeferredActorSpawnFromClass(
    WorldContextObject,                  // any UObject in the world
    TSubclassOf<AActor> Class,           // <-- from Fstruct_save.class_3_*
    FTransform SpawnTransform,           // <-- from Fstruct_save.transform_8_*
    ESpawnActorCollisionHandlingMethod CollisionHandlingOverride,  // AdjustIfPossibleButAlwaysSpawn
    AActor* Owner                        // the player (mainPlayer_C) for ownership
) -> AActor* (uninitialized)

// BP then SETS props on the returned actor (BP exec wires; for the inventory
// drop case the BP graph does NOT touch the returned actor before
// FinishSpawningActor -- it relies on Aprop_C::Init + loadData to populate)

UGameplayStatics::FinishSpawningActor(
    AActor* SpawnedActor,
    FTransform SpawnTransform
) -> AActor*
// triggers Aprop_C::Init() (BP UserConstructionScript), then
// ReceiveBeginPlay, then -- via the takeObj BP body -- loadData(Output)
```

### 2a. Class parameter source

`Fstruct_save.class_3_5267A5ED44C89294283B8CBBEC685F8A` at offset 0x0000
(8 bytes, `TSubclassOf<AActor>`).

This is set when the prop was originally picked up: `addObject(AActor* Actor,
int32 insertIndex1, bool& return, FString& err)` on `UpropInventory_C` records
`Actor->GetClass()` (via the `Aprop_C::getData` getter that writes
`Data.class_3 = self->GetClass()`).

**For the wire**: serialize as the FName string of the leaf class
(`Aprop_equipment_flashlight_C`, `Aprop_food_apple_C`, etc.). Receiver
resolves via `engine::FindClassByName(L"Aprop_equipment_flashlight_C")`
(the same path Stage-4 receiver uses for `FindByKeyString`).

### 2b. Transform parameter source

`Fstruct_save.transform_8_9DFFF09E48141807687A338593998A8A` at offset 0x0010
(0x30 bytes, FTransform = FQuat rot + FVector loc + FVector scale).

For drops, the BP graph **overrides** the transform JUST before
`takeObj` returns:
- Tap-drop (`simulateDrop(_, place=false, _)`): transform = player's
  hand socket world transform (the `mainPlayer.holding_actor`'s
  current world transform), nudged forward ~30cm via a camera-forward
  raycast that finds free space.
- Place-drop (`simulateDrop(_, place=true, _)`) via
  `precisionPlacemet_drop()`: transform = `mainPlayer.SpawnLocation`
  at offset 0x08F0 (FTransform), which is the BP placement ghost's
  preview transform set by the per-tick aiming code.
- Equipment drop: transform = player capsule centre minus a small
  forward+down offset (see `RemoveEquipment` body).

**For the wire**: send the full FTransform as 10 floats (rot quat 4 +
loc vec 3 + scale vec 3). Scale will almost always be (1,1,1) for these
props but include it for fidelity. The PropPose packet already wires
`FTransform`-shaped data — this is the same shape.

### 2c. Physics-initial-state parameter source

The spawned actor's `Aprop_C::Init()` (UserConstructionScript) sets:
- `Aprop_C.Static = false` (the inventory-stored prop is non-static)
- `Aprop_C.frozen = false`
- `Aprop_C.StaticMesh` enables physics simulation (`SetSimulatePhysics(true)`)
  AND wakes the body (`SetAllPhysicsLinearVelocity(0)`, `WakeAllRigidBodies`).

So at the moment of receive, the spawned actor is a SIMULATING rigid
body at rest. If the drop transform has the prop in mid-air, gravity
takes over and it falls naturally — same on both peers, deterministic
within ~one tick.

`Fstruct_save` also carries dynamic state arrays (`bools_12_*`, `floats_31_*`,
etc.) which `loadData` applies. These restore per-prop logic state
(burning flag on a torch, fuel level on a flashlight, etc.). For the
initial wire we IGNORE these — they're inventory-private state. After
the drop, normal Stage-4 PropPose updates carry the prop's transform;
the per-prop dynamic state (burning, light-on) is the in-hand world
effect path of Section 7 (separate `EntityEvent("itemState", ...)`).

---

## Section 3 — Key string generation

This is where the project-coop-physics-object-pickup memory's "Key
string is the save UUID baked into .sav" claim applies. The Key
trajectory of a dropped-from-inventory prop is:

```
T=0 (original world spawn, e.g. КПП-area suitcase loaded from save):
  Aprop_C::Init() runs.
  - If self->ResetKey == false AND save-load context set a Key already,
    self->Key = (the FName from the save).
  - If self->ResetKey == true OR no Key set, Init() calls into the
    mainGamemode's Key generator:
        mainGamemode_C calls KismetGuidLibrary::NewGuid (string at
        rva 0x1441abea8 in IDB) -> FName(NewGuidString).
  Either way: self->Key is now a stable FName whose ToString'd form
  is THE cross-peer identifier.

T=pickup (player E-presses on suitcase, hold-to-inventory branch):
  Aprop_C::getData(Fstruct_save& Data) is called.
  Data.key_64_* = self->Key
  The Fstruct_save is appended to UpropInventory_C.<items array>.
  The world actor is then K2_DestroyActor'd. Local-only operation:
  the OTHER peer's Stage-4 wire would see the actor go null and unbind.
  This is the world-pickup half of Phase 5N3.

T=drop (player Q-presses or container-extracts):
  UpropInventory_C::takeObj(idx) returns the Fstruct_save.
  The new world actor is SpawnActor'd, then loadData(Output) is called:
    Aprop_C::loadData(Fstruct_save Data, bool& return)
    -> self->Key = Data.key_64_*       (RESTORES original Key)
    -> self->propData = ... (data from the saved arrays applies)
    -> setKey(self->Key)               (registers in
                                        mainGamemode.keyObj_key/obj)
```

**Critical finding**: the Key on the dropped prop is the SAME Key
the prop had before pickup. So **if the OTHER peer also had the same
prop pre-pickup AND we wired the pickup event, the receiver could
just look up the Key in its local index**. But we cannot rely on this:

1. The original prop on peer B was already destroyed at pickup-time
   (Phase 5N3 pickup wire), so peer B has no local actor to "rebind".
2. Some props the host picks up may have spawned out of cooked content
   that peer B's gamemode didn't replicate (e.g. host's crafted item).
3. Some Keys are freshly generated by `NewGuid` and are NOT in peer B's
   keyObj table at all.

So the wire MUST carry the Key string explicitly in the PropSpawn
packet. The receiver SpawnActor's the same class at the same transform,
then calls `Aprop_C::setKey(receivedKeyFName)` to align Key on both
peers. From then on Stage-4 PropPose / PropRelease packets resolve
normally via `prop_wrap::FindByKeyString`.

`Aprop_C::setKey(FName Key)` IS BP-callable (declared in prop.hpp line
181) — it sets `self->Key` and updates
`mainGamemode.keyObj_key[]` / `keyObj_obj[]`. We dispatch it on the
receiver immediately after the SpawnActor, before any subsequent
PropPose can arrive.

---

## Section 4 — UFunction inventory (signatures + addresses)

| Class | UFunction | Signature | Addr | Role |
|---|---|---|---|---|
| AmainPlayer_C | InpActEvt_drop_K2Node_InputActionEvent_0 | `(FKey)` | BP bytecode (no native addr) | Q-key press handler |
| AmainPlayer_C | InpActEvt_drop_K2Node_InputActionEvent_1 | `(FKey)` | BP bytecode | Q-key alt handler |
| AmainPlayer_C | simulateDrop | `(bool dontWakeup, bool place, bool dontCollect)` | BP bytecode | THE drop routing core (4 branches A-D above) |
| AmainPlayer_C | dropGrabObject | `()` | BP bytecode | Path A — release physics grab |
| AmainPlayer_C | forceDrop | `()` | BP bytecode | Bypass for emergency-drop (death, kicked) |
| AmainPlayer_C | timeDrop / timeDropTimer | `()` | BP bytecode | Hold-Q-to-place timer |
| AmainPlayer_C | precisionPlacemet_drop | `()` | BP bytecode | Path-D variant — drop at SpawnLocation@0x08F0 |
| AmainPlayer_C | objectDropped (delegate) | `()` | BP bytecode | Fires AFTER simulateDrop sets the world body |
| AmainPlayer_C | pickupObject | `(FHitResult)` | BP bytecode | Inverse path — world→inventory |
| AmainPlayer_C | pickupObjectDirect | `(AActor*, UPrimitiveComponent*)` | BP bytecode | Inverse path (programmatic) |
| AmainPlayer_C | hotkeyAction | `(TEnumAsByte<enum_hotkeyAction::Type>, AActor*)` | BP bytecode | Hotbar-1..0 use/drop |
| AmainGamemode_C | RemoveEquipment | `(int32 Index, bool Drop, bool& return)` | BP bytecode | Path C — equipment slot drop |
| AmainGamemode_C | AddEquipment | `(Fstruct_save Data, bool& return)` | BP bytecode | Inverse — equipment slot fill |
| AmainGamemode_C | spawnPropThroughGamemode | `(FName prop, FTransform InputPin, int32 Amount, AActor*& actor)` | BP bytecode | Direct-spawn helper (cheat / world gen — NOT wire-relevant) |
| AmainGamemode_C | putObjectInventory | `(AActor* Actor, bool& return)` | BP bytecode | ~~Helper for moving a world actor INTO the player inventory~~ **DEAD CODE — measured 2026-07-24: zero callers game-wide.** Its body does do this (into `saveSlot.inventoryData`), which is why the row reads as a live path, but the live pickup verb is `mainPlayer::putObjectInventory2` → `GObjStack`. See `votv-player-inventory-two-layer-RE-2026-07-24.md` §4.1/§4.2 |
| AmainGamemode_C | getObjectFromKey | `(const FName ItemToFind, AActor*& Output)` | BP bytecode | Key → AActor* lookup over keyObj_key/keyObj_obj |
| Aprop_container_C | extract | `(int32 Index)` | BP bytecode | Path D — container extract |
| Aprop_container_C | getObject | `(int32 Index, bool customLoc, FVector Loc, AActor*& OutputPin)` | BP bytecode | Programmatic extract-with-location (spawns world actor + returns it) |
| Aprop_inventoryContainer_player_C | extract | `(int32 Index)` | BP bytecode | Override of Path D for player inventory |
| UpropInventory_C | takeObj | `(int32 Index, bool removeVol, bool& return, Fstruct_save& Output, AActor*& Object)` | BP bytecode | THE bottom call — does SpawnActor + loadData |
| UpropInventory_C | addObject | `(AActor* Actor, int32 insertIndex1, bool& return, FString& err)` | BP bytecode | Inverse — calls getData on Actor + appends Fstruct_save |
| UpropInventory_C | getObj | `(int32 Index, bool& return, Fstruct_save& Output)` | BP bytecode | Read-only access |
| Aprop_C | getData | `(Fstruct_save& Data)` | BP bytecode | Serialize self → Fstruct_save (all Aprop subclasses override to add their own state) |
| Aprop_C | loadData | `(Fstruct_save Data, bool& return)` | BP bytecode | Deserialize Fstruct_save → self (sets Key, propData, custom state) |
| Aprop_C | setKey | `(FName Key)` | BP bytecode | Assigns Key + registers in mainGamemode.keyObj_key/keyObj_obj table |
| Aprop_C | GetKey | `(FName& Key)` | BP bytecode | Read Key out-param |
| Aprop_C | getOnlyKey | `(FName& Key)` | BP bytecode | Read Key out-param (variant — may bypass parent class delegation) |
| Aprop_C | Init | `()` | BP bytecode | UserConstructionScript; runs on spawn; may NewGuid the Key if ResetKey=true |
| UGameplayStatics | BeginDeferredActorSpawnFromClass | `(UObject* World, TSubclassOf<AActor>, FTransform, ESpawnActorCollisionHandlingMethod, AActor* Owner) -> AActor*` | native exec thunk; FName string @ 0x144199b88 | The "Spawn" half of K2Node_SpawnActorFromClass |
| UGameplayStatics | FinishSpawningActor | `(AActor*, FTransform) -> AActor*` | native exec thunk; FName string @ 0x144199de0 | The "Finish" half |

**IDA renames performed this session**: none new (BP bytecode functions
have no native addresses to rename). The 4 FName-pool entries above had
explanatory `set_comments` added + `idb_save`'d so the next IDA session
sees what each string means in context.

---

## Section 5 — Proposed PropSpawn wire packet structure

```c++
// New reliable packet, sent on the same reliable channel as PropRelease
// + chat + EntityEvent (project-coop-chat-feed reliability layer).
//
// Wire layout (little-endian, packed; matches protocol v4 PropRelease shape):
struct PropSpawnPacket {
    uint8_t  type;              // PACKET_PROP_SPAWN = 0x?? (next free id)
    uint32_t senderPeerId;      // for echo suppression
    uint64_t sequence;          // reliable-channel seq

    // -- spawn identity ----------------------------------------
    uint16_t classNameLen;      // FName string length (UTF-16 code units)
    uint16_t classNameUtf16[classNameLen];  // e.g. L"Aprop_equipment_flashlight_C"
                                            // resolves via reflection::FindClass

    uint16_t keyStringLen;
    uint16_t keyStringUtf16[keyStringLen];  // the persistent cross-peer Key

    // -- spawn transform ---------------------------------------
    float    rotQuatX, rotQuatY, rotQuatZ, rotQuatW;
    float    locX, locY, locZ;
    float    scaleX, scaleY, scaleZ;        // almost always 1,1,1; include for fidelity

    // -- initial physics state ---------------------------------
    uint8_t  physFlags;         // bit 0: bSimulatePhysics (always 1 for inv drops);
                                // bit 1: bSleeping (start at rest vs awake);
                                // bit 2: bIsHeavy (data-driven from propData.heavy --
                                //         determines which Stage-4 grab observer
                                //         the receiver should expect, PHC vs PCC)
                                // bit 3: bFrozen (Aprop_C.frozen -- quest-lock)
                                // bits 4-7: reserved
    float    initLinVelX, initLinVelY, initLinVelZ;   // usually 0; non-zero for
                                                      // throw-from-inventory (unused
                                                      // for tap-drop)
    float    initAngVelX, initAngVelY, initAngVelZ;   // usually 0
};
// Size: 1 + 4 + 8 + (2 + 2*classNameLen) + (2 + 2*keyStringLen) + 40 + 1 + 24
//     = ~ 80 + 2*(classNameLen + keyStringLen) bytes
//     ~ 130 bytes for a typical "Aprop_equipment_flashlight_C" + 22-char Key.
```

**Why we DO NOT serialize the full Fstruct_save**:
- It is per-peer private state (user rule project-coop-inventory-private).
- The bools/floats/ints arrays carry inventory-side state the other peer
  doesn't need to know about.
- The class + transform + key + physics-state subset is SUFFICIENT to
  spawn a matching prop that subsequent PropPose updates can drive.
- World-effect state (flashlight on/off, torch burning, etc.) goes
  through the separate `EntityEvent("itemState", propKey, stateBitmask)`
  channel (Section 7).

**Why NOT use Stage-4's PropPose alone as the spawn carrier**:
- PropPose is unreliable / sequenced (per protocol v4); spawn MUST be
  reliable (one missed packet = permanent invisible prop on the
  other peer).
- PropPose assumes the receiver has the prop indexed — by class +
  Key — which is the chicken-and-egg this packet breaks.

---

## Section 6 — Observer install point

We have TWO viable hook sites for the sender. The choice depends on
which one gives the cleanest "spawn just happened, here are the params"
snapshot.

### Option 6a (RECOMMENDED): POST-hook `UpropInventory_C::takeObj`

`takeObj` is the BP bottom of the drop pipeline — every drop path
(simulateDrop A-D, equipment, container, hotkey) eventually hits this
one function. PE observer:

```
ProcessEvent hook fires with function=UpropInventory_C::takeObj
On POST (after BP body returns, before the caller resumes):
    args layout (per UFunction FProperty walk):
      [0] Index            (int32)
      [1] removeVol        (bool)
      [2] return           (bool, out)  -- success flag
      [3] Output           (Fstruct_save, out)  -- the saved state
      [4] Object           (AActor*, out)       -- the newly spawned actor

    if return == true && Object != nullptr:
        cls    = reflection::ClassNameOf(Object->GetClass())
        keyStr = Output.key_64_*.ToString()    or  Aprop_C::GetKey(Object)
        xform  = K2_GetActorTransform(Object)
        physBits = encode(
          /*sim*/  is_static(Output.class_3_*) ? 0 : 1,
          /*sleep*/0,
          /*heavy*/Output.bytes... contains heavy bit? (or re-read from Object->propData.heavy),
          /*frozen*/0
        )
        session.SendPropSpawn({cls, keyStr, xform, physBits, 0vel, 0avel})
        g_remoteProps[keyStr] = Object  // cache for echo suppression
```

This catches EVERY drop path uniformly. ✓

**Crash-safety**: the FProperty walk is the same one Stage-4 already
uses for PropPose argument marshaling — no new reflection code needed.
The out-params are settled at POST time.

### Option 6b: PRE-hook on the 3 known callers (simulateDrop /
RemoveEquipment / Aprop_container_C::extract)

More observer surface (3 hooks not 1), more chance of missing a path
(e.g. cheat-menu spawn would slip), and we'd have to read mainPlayer
state to derive the spawn transform ourselves. NOT recommended.

### Receiver-side install

The reliable channel handler dispatches PropSpawn to a new
`coop::OnPropSpawn(packet)`:

```
coop::OnPropSpawn(PropSpawnPacket pkt):
  // Echo-suppress: did we just SEND this from a takeObj POST?
  if (g_recentSentSpawns.contains(pkt.keyString)) return;

  // Resolve class
  UClass* cls = reflection::FindClassByName(pkt.className);
  if (!cls) { LOG("PropSpawn: unknown class %S", pkt.className); return; }

  // Build FTransform
  FTransform xform;
  xform.Rotation = FQuat(pkt.rotQuatX,Y,Z,W);
  xform.Translation = FVector(pkt.locX,Y,Z);
  xform.Scale3D = FVector(pkt.scaleX,Y,Z);

  // SpawnActor
  AActor* spawned = engine::SpawnActorDeferred(cls, xform, /*owner*/nullptr);
  if (!spawned) return;

  // CRITICAL: setKey BEFORE FinishSpawning so Init+loadData see the right
  // Key (no NewGuid generation). We use the BP-callable setKey via PE.
  reflection::CallUFunction(spawned, L"setKey", &keyFName);

  // FinishSpawning -- runs UserConstructionScript + loadData (with empty
  // Fstruct_save -- the saved per-peer state is private to the sender;
  // we accept the prop will have default per-prop state).
  engine::FinishSpawningActor(spawned, xform);

  // Apply initial physics flags
  if (pkt.physFlags & 0x01) {
    reflection::CallUFunction(GetStaticMesh(spawned), L"SetSimulatePhysics", &(bool=true));
  }
  if (pkt.physFlags & 0x08) {  // frozen
    // set Aprop_C.frozen byte directly via reflection
  }
  if (any(pkt.initLinVel)) {
    reflection::CallUFunction(GetStaticMesh(spawned), L"SetPhysicsLinearVelocity",
                              &lvel, &bAddToCurrent=false);
  }
  if (any(pkt.initAngVel)) {
    reflection::CallUFunction(GetStaticMesh(spawned), L"SetPhysicsAngularVelocityInDegrees",
                              &avel, &bAddToCurrent=false);
  }
  // Done. Stage-4 PropPose updates with this Key now resolve normally
  // via prop_wrap::FindByKeyString.
```

**Echo suppression**: the SENDER must NOT process its own PropSpawn
echo (which can happen if we relay through host). Add a small
fixed-window set keyed by (peerId, sequence). Same shape as the
Stage-4 echo guard.

**Race with PropPose**: a subsequent PropPose packet with the same
Key may arrive BEFORE the receiver's SpawnActor completes (one-frame
race). The Stage-4 receiver already handles "unknown Key" gracefully
(it does FindByKeyString again next frame). After SpawnActor completes
the lookup succeeds. No protocol change needed.

---

## Section 7 — Receiver match steps + in-hand world effect (light toggle)

### 7a. The match steps (summary of Section 6 receiver):

1. **Class resolution**: `reflection::FindClassByName(pkt.className)` —
   the FNAME → UClass lookup via GUObjectArray (same pattern Stage-4
   uses for `prop_C` base class detection in `IsDescendantOfProp`).
2. **Deferred SpawnActor**: `BeginDeferredActorSpawnFromClass(world,
   cls, xform, AdjustIfPossibleButAlwaysSpawn, owner=nullptr)`.
3. **setKey before FinishSpawning**: this ensures `Aprop_C::Init()`
   (UserConstructionScript) runs with the correct Key already in
   place. Otherwise `Init` may overwrite via NewGuid (when
   `ResetKey=true`).
4. **FinishSpawningActor**: runs UCS, ReceiveBeginPlay, registers in
   `mainGamemode.allProps` + `keyObj_key/obj`.
5. **Apply physics flags**: `SetSimulatePhysics(true)`,
   `SetPhysicsLinearVelocity/AngularVelocity` if non-zero.
6. **Done** — Stage-4 PropPose updates resolve this Key from now on.

### 7b. Light/torch toggle (in-hand world effect — SEPARATE from Bug C)

VOTV's flashlight is structured as TWO actor families:
- `Aprop_equipment_flashlight_C` derives `Aprop_equipment_C` derives
  `Aprop_C` — this is the WORLD prop, what spawns when you drop the
  flashlight from inventory. Has `equip(bool& deleted)` and a typical
  prop dispatch surface.
- The IN-HAND light is NOT a separate actor. When equipped, the flashlight
  becomes:
  - An FName entry in `mainPlayer.equipment[]` (TArray<FName> @ 0x0B58)
  - The `mainPlayer.hasFlashlight` bool (@ 0x0CC2)
  - A USpotLightComponent on the mainPlayer pawn itself (driven by
    `mainPlayer::flashlightStateChanged(USpotLightComponent* Light, bool Visible)`
    delegate at offset 0x0AC8)

So when peer A turns ON their held flashlight, the WORLD effect is:
- peer A's `mainPlayer.flashlightMode` (uint8 @ 0x0C79) changes
- peer A's mainPlayer-attached SpotLight becomes Visible
- the cone illuminates the world from peer A's hand

For peer B's puppet of peer A to show the same cone, we need:
- A `mainPlayer.equipment[]` FName-level event (the puppet's "equipped"
  state) OR a simpler per-pawn `flashlightOn` bool replicated
- The puppet pawn (a `mainPlayer_C` orphan) ALREADY has the SpotLight
  component built into its BP. So toggling
  `mainPlayer.flashlightMode = X` on the puppet via reflection +
  calling `mainPlayer::flashlightStateChanged(spotComp, true)` ought
  to make it glow.

**This is PER-PAWN STATE, NOT per-prop state.** It lives in the
puppet pose stream (Stage-2/3 RemotePlayer wire), NOT in the
PropPose/PropSpawn wire. The puppet pose packet can carry an extra
`equipFlags : uint16` bitmask (flashlight on, glasses, krampushat,
metaldetec…) — that's a clean Stage-5-ish extension separate from
Bug C.

So **Bug C only covers PROP world-spawn** (the world actor is a fresh
Aprop_X_C instance). The in-hand light toggle is a separate replication
channel — properly part of Phase 5N3's `EntityEvent("itemState", ...)`
but applied to the PLAYER pawn, not to a prop. The wire packet for THAT
should be a `PlayerEquipmentEventPacket` (eq state bitmask + pawn id),
NOT a PropSpawn.

This is the right separation per architectural principle 7: world
events vs per-pawn state are different layers.

---

## Section 8 — Open questions for Stage-5 implementation

1. **`takeObj` POST hook param marshaling**: confirm via PE observer
   that the out-params (`return`, `Output`, `Object`) are accessible
   at POST time (UE4.27 typically YES — PE returns after writing
   out-params to the caller frame). One-shot autonomous verify with
   the existing harness: trigger an inventory drop in autotest, log
   the marshaled `Object` ptr + `Output.key_64_*.ToString()`.
2. **Class FName resolution on receiver**: `Aprop_equipment_flashlight_C`
   et al — verify these UClass pointers exist on a fresh world load
   on both peers (cooked content registration). If not, fall back to
   on-demand `LoadClass` via SoftObjectPath.
3. **`Aprop_C::Init()` Key-overwrite race**: confirm `setKey` BEFORE
   `FinishSpawningActor` actually prevents NewGuid generation in
   Init. Backup: call setKey TWICE (before + after Finish). RULE 1
   crutch flag: only acceptable if we prove Init mid-step needs both.
4. **Receiver: don't double-spawn on echo**: maintain a 256-entry LRU
   keyed on `(senderPeerId, sequence)` to drop echo packets in the
   host-relay topology.
5. **Per-prop dynamic state restoration on receiver**: a torch dropped
   while burning vs not-burning. For the FIRST cut we accept the
   default (not-burning); for completeness we extend the packet with
   a small `dynStateBitmask : uint32` carrying the most user-visible
   per-prop bits. This is iterative — start without, add as
   user-visible bugs surface.

---

## Cross-refs

- `src/votv-coop/include/ue_wrap/sdk_profile.h` — offsets + class strings
- `src/votv-coop/include/ue_wrap/prop.h` — Aprop_C accessor API (Key, transform)
- `Game_0.9.0n/.../CXXHeaderDump/prop.hpp` — full Aprop_C UFunction list
- `Game_0.9.0n/.../CXXHeaderDump/mainPlayer.hpp` — drop entry points
- `Game_0.9.0n/.../CXXHeaderDump/mainGamemode.hpp` — keyObj table + RemoveEquipment
- `Game_0.9.0n/.../CXXHeaderDump/prop_container.hpp` — extract / getObject
- `Game_0.9.0n/.../CXXHeaderDump/prop_inventoryContainer_player.hpp` — player override
- `Game_0.9.0n/.../CXXHeaderDump/propInventory.hpp` — takeObj / addObject
- `Game_0.9.0n/.../CXXHeaderDump/struct_save.hpp` — Fstruct_save schema
- `Game_0.9.0n/.../CXXHeaderDump/struct_prop.hpp` — Fstruct_prop (heavy flag, mesh)
- `research/findings/physics-grab/votv-physics-interaction-deep-re-2026-05-23.md` — Stage 1-4 RE
- `research/findings/mta/mta-object-pickup-sync-2026-05-23.md` — MTA precedent
- [[project-physics-object-pickup]] — Stage 1-4 ship state
- [[project-coop-inventory-private]] — inventories per-peer-private rule
- IDB comments added at 0x144199b88 / 0x144199de0 / 0x1441ac378 / 0x1441abea8
