# VOTV order + drone-delivery pipeline — measured RE (2026-07-22)

Scope: where an ORDER comes from, what the drone does with it, where the delivered items
physically land, what the sack is, and which of those seams a coop sync lane can actually hook.
Written because a coop sync attempt aimed at the wrong entity and failed hands-on.

Confidence tags on EVERY claim: `[V]` measured (citation inline) · `[RD]` RE-derived (a chain of
`[V]` facts, no single citation) · `[?]` unverified / cannot be settled statically.

Method: `kismet-analyzer to-json` + `gen-cfg` over the cooked `.uasset`s
(`research/pak_re/extracted/VotV/Content/...`) plus the CXX header dump
(`Game_0.9.0n_HOST/.../CXXHeaderDump/*.hpp`). Bytecode offsets quoted below are the CFG
byte offsets from `kismet-analyzer gen-cfg`; statement indices `[n]` are the sequential index in
the `ScriptBytecode` array of that export. Both are reproducible from those two commands.

---

## 0. TL;DR — the three corrections

1. **`prop_dronesack_C` absolutely exists at runtime.** It is `Aprop_dronesack_C : Aprop_C`
   with its own SCS components and its own `container` pointer `[V]` (`prop_dronesack.hpp`
   class decl + `0x0380 container : Aprop_inventoryContainer_drone_C*`). `ignoreSave` returning
   `EX_True` `[V]` (`prop_dronesack.uasset` export `ignoreSave` `[0] LocalOutVariable[ignoreSave]
   := EX_True`) means only "the save system skips me" — it says nothing about runtime existence.
   The earlier "does not exist at runtime" conclusion is **WRONG** and is retracted here.

2. **The delivery target IS the persistent `drone_InventoryContainer` — but the drone does not
   find it by that name.** The delivered items land in
   `drone.container.propInventory` `[V]`, and `drone.container` is resolved at BeginPlay by
   `gamemode.getObjectFromKey(n'droneContainer')` `[V]` (drone ubergraph @14084,
   `EX_NameConst droneContainer`) — while the container actor's CDO key is
   `drone_InventoryContainer` `[V]` (`Default__prop_inventoryContainer_drone_C` property
   `key = "drone_InventoryContainer"`). Those are two different FNames, and the name
   `droneContainer` appears in **exactly one** cooked asset in the whole `Content` tree —
   `objects/drone.uasset` itself `[V]` (`grep -rl droneContainer Content/` → 1 hit) — i.e. it is
   never registered. So the lookup fails and the drone **spawns a fresh
   `prop_inventoryContainer_drone_C`** `[V]` (drone ubergraph block @14206,
   `BeginDeferredActorSpawnFromClass(prop_inventoryContainer_drone_C, GetTransform(), 0)` →
   `container := FinishSpawningActor(...)`). That fresh actor carries the same CDO key, so it can
   still BE the persistent one from the save system's point of view — see §5, and the open
   question O-1 which static analysis cannot close.

   The `prop_dronesack_C` looks the container up with the **correct** key:
   `getObjectFromKey(n'drone_InventoryContainer')` `[V]` (prop_dronesack ubergraph `[11]`).
   The two blueprints disagree about the name. This asymmetry is measured, not inferred.

3. **The mod's own comment is wrong about this too.** `ue_wrap/devices/drone.cpp:321` says the
   container is "keyed 'droneContainer'". It is keyed `drone_InventoryContainer`; `droneContainer`
   is only the (failing) lookup string. Cosmetic, but it is the sentence that would mislead the
   next reader.

---

## 1. The entities

| Entity | Class / type | Where it lives | Save | Notes |
|---|---|---|---|---|
| Drone | `Adrone_C : AActor` `[V]` (`drone.hpp`) | world | `getData` key = `n'drone'` `[V]` (`drone.getData [10]`) | `container@0x04F8`, `order@0x0348 : Fstruct_storeOrder`, `hasOrder@0x0360`, `hasSack@0x0501`, `canTakeOff@0x0500`, `flyingType@0x0300`, `lastSpawn@0x03C0` `[V]` (`drone.hpp`) |
| Delivery container | `Aprop_inventoryContainer_drone_C : Aprop_container_C : Aprop_C` `[V]` (`prop_inventoryContainer_drone.hpp`) | spawned by the drone at BeginPlay `[V]` | CDO `key = drone_InventoryContainer`, `static = true` `[V]`; `gatherDataFromKey → gather=TRUE, loadTransform=FALSE` `[V]`; `skipPreDelete → TRUE` `[V]` | CDO `propInventory.index = 1`, `propInventory.infinite = true` `[V]` (`propInventory_GEN_VARIABLE` export data) |
| Its inventory | `UpropInventory_C : UActorComponent` `[V]` (`propInventory.hpp`) | component | — | `Index@0xB0`, `maxVol@0xB4`, `currVol@0xB8`, `obj@0xC0 (UStaticMeshComponent*)`, `Mass@0xC8`, `Owner@0xD0 (Aprop_container_C*)`, `infinite@0xF8`, `Player@0xF9` `[V]` |
| Sack (world) | `Aprop_dronesack_C : Aprop_C` `[V]` | spawned below the drone by `dropSack` `[V]` | `ignoreSave → TRUE` `[V]` | `container@0x0380`, `takenByDrone@0x0388`, `Actor@0x0390`, `comp@0x0398` `[V]` |
| Sack (visual, on the drone) | `sack : USkeletalMeshComponent@0x0250` + `sackConstraint : UPhysicsConstraintComponent@0x0258` + `sackPhys : USphereComponent@0x0260` + `sackHitbox : UBoxComponent@0x0248` `[V]` (`drone.hpp`) | drone components | — | see §7 |
| Order queue | `saveSlot.orders : TArray<Fstruct_storeOrder>@0x0490` `[V]` (`saveSlot.hpp`) | the save object | persisted | `Fstruct_storeOrder { TArray<Fstruct_store> items; float time; }` size 0x14 `[V]` (`struct_storeOrder.hpp`) |
| Daily-delivery latch | `saveSlot.dailyDelivery : bool@0x0E40` `[V]` (`saveSlot.hpp`) | the save object | persisted | |
| Order UI | `Uui_laptop_C` (`gamemode.laptop@0x0448` `[V]`) | widget | — | owns `cart`, `slots_cart`, `slots_order`, `orderN`, `storePrice` |
| Key registry | `mainGamemode.keyObj_key : TArray<FName>@0x0828` + `keyObj_obj : TArray<AActor*>@0x0838` `[V]` | gamemode | runtime-only | written ONLY by `lib_C::assignKey` `[V]`; read by `getObjectFromKey` `[V]` |

**`GObjStack`.** Confirms the prior session's measurement: `saveSlot.GObjStack :
TArray<Fstruct_mObject>@0x0198` `[V]` (`saveSlot.hpp`), and `propInventory::addObject` operates on
`owner.gamemode.saveSlot.GObjStack[Index].obj` `[V]` (`propInventory.addObject [4]`:
`Array_LastIndex(((owner)->gamemode)->saveSlot)->GObjStack[InstanceVariable[index]].obj_11_...)`).
`propInventory::init` appends a fresh `Fstruct_mObject` and takes the new index only when
`index < 0` `[V]` (`init [11] GreaterEqual_IntInt(index, 0)` → `[12] jumpIfNot` → `[14]
Array_Add(GObjStack, ...)` → `[15] index := Array_Add_ReturnValue`), and `init` is also what sets
`Owner`: `[1] InstanceVariable[owner] := LocalVariable[owner]` `[V]`. `init` is called from
`prop_container::spawned [4]` as `(propInventory)->LocalVirtualFunction init(StaticMesh, EX_Self)` `[V]`.

---

## 2. The two fire paths

The user's statement — "the automatic daily arrival is a kind of order but actually NOT — they have
different fire paths" — is **half right, and the half that is right matters**. The two paths have
different PRODUCERS and different bookkeeping, but they **converge on the same drone entry point
and the same queue**. There is no second delivery mechanism.

```
(a) SHOP ORDER (player)                     (b) AUTOMATIC / DAILY
    ui_laptop cart UI                            daynightCycle::func_newHour(time)
      |                                            | hour >= 6 AND NOT saveSlot.dailyDelivery
      | ubergraph [219]                            | daynightCycle::"Make Default Order"(out order)
      v                                            v
    ui_laptop::makeAnOrder(order, automatic=FALSE)  ui_laptop::makeAnOrder(order, automatic=TRUE)
      \______________________________  ____________________________/
                                     \/
                    [0] isAutomatic := automatic
                    [1] addOrderCart(order)   -> saveSlot.orders.Add(order)      <-- THE QUEUE
                                              -> create uicomp_shopOrderSlot_C widget
                                              -> slots_order.Add(widget)
                                     |
                automatic? --yes-->  [3] Array_Get(saveSlot.orders, 0)
                                     [4] gamemode.drone.sendShop(orders[0])   ; RETURN
                                     |
                     --no-->  [8]  stats.items_bought += cart.Num
                              [9]  cart.Clear()
                              [14-19] remove + clear slots_cart widgets
                              [20-23] orderN++ ; storePrice = 0
                              [24] Array_Get(saveSlot.orders, 0)
                              [25] gamemode.drone.sendShop(orders[0])
                              [26-32] update txt_cartSize / txt_price
```
All of the above `[V]` — `ui_laptop.uasset` exports `makeAnOrder` (statements as numbered) and
`addOrderCart` (`[0] Array_Add(gamemode.saveSlot.orders, NewItem)`), and `daynightCycle.uasset`
export `func_newHour` (`[2] GreaterEqual_IntInt(hour, 6)`, `[4] jumpIfNot cond=saveSlot.dailyDelivery`,
`[6] "Make Default Order"`, `[7] makeAnOrder(order, EX_True)`, `[8] saveSlot.dailyDelivery := EX_True`).

**Third producer, same path:** `trigger_eventer` also calls
`gamemode.laptop.makeAnOrder(<literal order>, EX_True)` at two sites `[V]`
(`trigger_eventer` ubergraph `[331]`, `[354]`) — event-scripted deliveries reuse the automatic path.

**Where they DIVERGE (exhaustive):** only in (i) who builds the `Fstruct_storeOrder`, (ii) the
`items_bought` stat, (iii) clearing the cart + cart widgets, (iv) `orderN` / `storePrice` / the two
UI texts. Nothing on the drone side differs. `[V]` (the `automatic` branch is a single
`jumpIfNot ->271` at `makeAnOrder [2]`, and both arms end in the identical `sendShop(orders[0])`).

**Both paths send `orders[0]`, not the order they just added** `[V]` (`makeAnOrder [3]` and `[24]`
are both `Array_Get(..., 0, ...)`). So with a non-empty queue the drone is always dispatched for
the head of the queue; a newly-added order simply lands behind it.

**Payload of the daily order** `[V]` (`daynightCycle::"Make Default Order"`): an array `a` of
`Fstruct_store` rows, unconditionally `prop_reelbox_C`, `prop_reel_small_C`, `prop_reel_big_C`, and
`prop_C` with `asProp = n'reelcase_1'`; plus conditional rows behind `sendDriveBox(gamemode)` →
`prop_box_C`, and separately `prop_drive_C`, `prop_floppyDisc_Wh_C`, `prop_food_mre_C` (each behind
its own `pushFlow`-gated branch). `time = 0`. Every row has `price = 0`, `size = 1`,
`parseRowNameToObject = false`.

**Payload of a shop order:** the cart rows the player assembled in `ui_laptop` `[RD]` (the cart is
`InstanceVariable[cart]`, and `makeAnOrder` clears it right after queueing).

---

## 3. `sendShop` → flight → `compileOrder`: the drone state machine

`sendShop(order)` is a BP event; its body is `ExecuteUbergraph_drone(13422)` `[V]`.

```
13422: EX_JumpIfNot 13437 cond=InstanceVariable[active]      ; if ALREADY active -> 13436 popFlow (abort)
13437: order := K2Node_CustomEvent_order                      ; latch the order onto the drone
13464: EX_JumpIfNot 13908 cond=gamemode.radiotower.isBroken   ; broken tower -> error/email branch @13522
13908: calledError := false
13919: EX_LocalVirtualFunction beginFly
13933: hasOrder := true
13944: EX_LocalVirtualFunction checkOrders
13958: EX_PopExecutionFlow
```
`[V]` (CFG blocks @13422, @13437, @13908).

`checkOrders` `[V]` (own export, 22 statements):
```
[2] Array_IsValidIndex(gamemode.saveSlot.orders, 0)
[3] jumpIfNot -> (else branch)
[4]   flyingType := 0
[5]   Array_Get(gamemode.saveSlot.orders, 0, item)
[6]   LocalVirtualFunction sendShop(item)            ; re-entrant; guarded by `active`
[8-16] for each w in gamemode.laptop.slots_order: w.upd()     ; refresh the order widgets
else:
[17]  flyingType := -1
[18]  active := false
```
`checkOrders` is also called on the return leg: `10308: active := false; 10319: checkOrders`
`[V]` (CFG block @10308), i.e. after a completed run the drone re-checks the queue and, if another
order is pending, dispatches itself again.

**Arrival → `compileOrder`.** During `ReceiveTick` the drone tests distance/velocity/`canTakeOff`
and, on arrival, runs `soundAlarm` (@12535) and then block @12554:
```
12554: Array_IsValidIndex(order.items, 0)
12619: EX_PopExecutionFlowIfNot <that>
12629: EX_LocalVirtualFunction compileOrder
12643: order := struct_storeOrder{ [], 0 }               ; clear the latched order
12694: hasOrder := false
12705: (gamemode.laptop)->EX_LocalVirtualFunction removeOrderCart
```
`[V]` (CFG blocks @12554, @12629).

`ui_laptop::removeOrderCart` `[V]` (own export, 7 statements):
```
[0] Array_Get(gamemode.laptop.slots_order, 0, w)
[1] w.RemoveFromParent()
[2] Array_Remove(gamemode.saveSlot.orders, 0)
[3] Array_Remove(gamemode.laptop.slots_order, 0)
[4] lib.recountChildren(scrollbox_orders, self)
```
So the queue is FIFO and the head is popped **by the laptop widget**, at delivery time, not by the
drone. `[V]`

---

## 4. `compileOrder` — where ordered items actually land

Full statement-level trace of `drone.compileOrder` (113 statements, 4014 bytes). The function
loops over `order.items` (`Temp_int_Loop_Counter_Variable` / `Temp_int_Array_Index_Variable_1`,
`[4]-[6]`).

Per item:

**Branch A — `items[i].object == prop_C`** (`[11] EqualEqual_ClassClass(item.object, prop_C)`,
`[12] jumpIfNot ->1977`):
```
[13-15] spawn prop_C deferred at box.GetComponentLocation()
[18]    SetNamePropertyByName(spawned, n'name', <asProp or name, per item.parseRowNameToObject>)
[21-22] spwnd := FinishSpawningActor(...)
[25-26] if <that name> == n'None' -> bail out of this item
```
i.e. a generic `prop_C` skinned by the props DataTable row name.

**Branch B — otherwise** (`[57]-[63]`): spawn `items[i].object` (the concrete class) deferred at
`box.GetComponentLocation()` and `FinishSpawningActor`.

**Then the middleman hop** (`[64]-[75]`), applied to the Branch-B spawn:
```
[64] K2Node_DynamicCast_AsInt_Coms := ObjToInterfaceCast<int_coms>(spawned)
[66] jumpIfNot -> skip
[67] (iface)->EX_LocalVirtualFunction intComs_storeMiddleman(out actor, out actors)
[68] Array_IsValidIndex(actors, 0)
[69] jumpIfNot -> single-actor branch
[70]   manyObjects := actors
[71]   manyObj    := true
       ...
[73] IsValid(actor)
[75]   spwnd := actor
```
So a "box" product (e.g. `prop_reelbox_C`) implements `int_coms::intComs_storeMiddleman` and hands
back **either** one replacement actor **or** an array of actors — this is what "one ordered item
expands into N physical items" means. `[V]`

**The two `addObject` call sites — and they have the SAME target:**
```
[40] ((InstanceVariable[container])->InstanceVariable[propInventory])
         ->EX_LocalVirtualFunction addObject(manyObjects[i], 0, out CallFunc_addObject_return,
                                             out CallFunc_addObject_err)
[42-43] lastSpawn := GetObjectClass(manyObjects[i])
[44] popFlowIfNot CallFunc_addObject_return
[46] (manyObjects[i])->EX_VirtualFunction K2_DestroyActor()

[51] ((InstanceVariable[container])->InstanceVariable[propInventory])
         ->EX_LocalVirtualFunction addObject(spwnd, 0, out CallFunc_addObject_return_1,
                                             out CallFunc_addObject_err_1)
[52-53] lastSpawn := GetObjectClass(spwnd)
[54] popFlowIfNot CallFunc_addObject_return_1
[55] (spwnd)->EX_VirtualFunction K2_DestroyActor()
```
`[V]`. So:

* **`CallFunc_addObject_return` (site `[40]`) = the `manyObj` loop** over `manyObjects` — the
  middleman's array, one `addObject` per expanded item.
* **`CallFunc_addObject_return_1` (site `[51]`) = the single-object case** — one `addObject` for
  `spwnd`.
* `manyObj : bool` (`[1]` initialised `false`, `[71]` set `true`) selects between them
  (`[31] jumpIfNot ->1792 cond=manyObj`). `manyObjects : TArray<AActor*>` holds the middleman's
  output; it is cleared per item (`[101]-[102]`). `[V]`
* **Neither branch is "pack into a box".** Both are the same destination. The only difference is
  cardinality. `[V]`
* **The live actor is DESTROYED on success.** `popFlowIfNot <return>` then `K2_DestroyActor` means
  "if addObject succeeded, destroy the world actor" — the item now exists only as an
  `Fstruct_save` record inside `GObjStack[container.propInventory.Index].obj`. `[V]`

**The Context of both `addObject` calls is `drone.container.propInventory`.** Not the drone's own
component (the drone has none), not the sack's, not a per-delivery spawned container. `[V]`

**`sendName` side-effects** (`[86]`, `[99]`, `[109]`): if the spawn implements `int_player`, it is
sent the item's row name. `[V]` Cosmetic for our purposes.

---

## 5. The container lifecycle — and the name mismatch

Drone `ReceiveBeginPlay` → `ExecuteUbergraph_drone(13963)` `[V]`, which pushes four branches:
13988 (`setPickupLocation`), **14003 (the container resolve)**, 14362 (a 2 s `Delay`), 14417
(bind `droneSackAnim`). `[V]` (CFG blocks @13963, @13968).

Block @14003 `[V]`:
```
14003: lib.getMainGamemode(self, out gm)
14049: (gm)->EX_LocalVirtualFunction getObjectFromKey
14084:      EX_NameConst droneContainer                       <-- THE LOOKUP NAME
14097:      out CallFunc_getObjectFromKey_Output
14107: cast<prop_inventoryContainer_drone_C>(Output)
14172: EX_JumpIfNot 14206
       (success) container := <cast result>
14206: (failure) GetTransform() ;
       BeginDeferredActorSpawnFromClass(self, prop_inventoryContainer_drone_C, transform, 0, null)
       FinishSpawningActor(...) ; container := <spawned>
```

`getObjectFromKey(ItemToFind)` is a linear `Array_Find(keyObj_key, ItemToFind)` → `keyObj_obj[ind]`,
returning `EX_NoObject` on miss `[V]` (`mainGamemode.getObjectFromKey`, 11 statements). The ONLY
writer of `keyObj_key`/`keyObj_obj` in the whole cooked corpus is `lib_C::assignKey` `[V]`
(`grep -rl keyObj_key Content/` → `lib.uasset`, `mainGamemode.uasset`, `propSpawner_editor.uasset`;
of those, `mainGamemode` only Set/Remove/Find (in `ExecuteUbergraph_mainGamemode` +
`getObjectFromKey`), `propSpawner_editor` only `Array_Contains`, and `lib::assignKey [25]-[27]`
does the two `Array_Add`s). `assignKey` is called from `prop_C::getKey` with the actor's own
`key` property `[V]` (`prop.getKey [1]`).

Therefore:
* Nothing anywhere registers the name `droneContainer`. `[V]` (grep: the string exists only in
  `drone.uasset`.)
* The drone's BeginPlay lookup **always misses**, and the drone **always spawns a fresh
  container**. `[RD]` — derived from three `[V]` facts (the lookup name, the registry's only
  writer, the grep); the only way it could be false is if the name were registered by native C++
  outside the BP corpus, which `[?]` I did not disprove.
* The sack's BeginPlay lookup uses `drone_InventoryContainer` `[V]`, which IS the CDO key, so it
  resolves **iff** some code path has already called `getKey()` on the container (that is the only
  thing that registers it).

**Who calls `getKey()` on it:** the save loader. `mainGamemode::loadObjects` runs a pre-pass over
existing world actors `[V]` (`loadObjects [295]-[321]`):
```
[298] (obj as int_save)->gatherDataFromKey(out gather, out loadTransform)
[299] jumpIfNot -> (not a gatherer)
[300]   existingGatherers.Add(obj)
[304]   (obj as int_objects)->getKey(out key)          <-- REGISTERS obj under its key
[305]   existingGatherersKeys.Add(key)
      (not a gatherer):
[312]   (obj as int_save)->skipPreDelete(out skip)
[313]   jumpIfNot -> [315] obj.K2_DestroyActor()       <-- otherwise the loader WIPES it
```
and then, per saved record `[V]` (`loadObjects [232]-[259]`):
```
[232-233] spawn the record's class at the record's transform
[241] (spawned as int_save)->gatherDataFromKey(out gather, out loadTransform)
[242] jumpIfNot -> normal path ([261] onwards: loadData on the spawned actor + dupe-check by key)
[243]   ind := Array_Find(existingGatherersKeys, record.key)
[244]   Array_Get(existingGatherers, ind, out existing)
[247]   jumpIfNot -> (cast failed)
[248]     (existing as int_save)->loadData(record, out ret)     <-- data goes to the EXISTING actor
[249]   spawned.K2_DestroyActor()                               <-- the temp spawn is thrown away
[250]   popFlowIfNot loadTransform ; ([257] would SetActorTransform on `existing`)
```
`prop_inventoryContainer_drone_C` returns `gather = TRUE, loadTransform = FALSE` `[V]` and
`skipPreDelete = TRUE` `[V]`, so it is a **gatherer**: the loader never respawns it, it finds the
live one by key and pushes `loadData` into it, and never moves it.

`prop_container::loadData` then writes the GObjStack slot number `[V]` (own export, 12 statements):
```
[3] Array_Get(data.ints, 0, out mInt)
[4] Array_Get(mInt.ints, 0, out i)
[5] (propInventory)->InstanceVariable[index] := i
[6] Array_Get(data.names, 2, out mName)
[7] nameData := mName.vectors
```
— three unguarded statements, no `Array_IsValidIndex`, no branch anywhere in the function.
Confirms the prior session's measurement exactly. The symmetric writer is `prop_container::getData
[1]-[4]`: `ints = [ [propInventory.index] ]` `[V]`.

**Open question O-1 (cannot be settled statically).** Whether the container the drone spawns is
present in `existingGatherers` depends on whether **drone BeginPlay runs before `loadObjects`'
pre-pass**. If the drone is a level-placed actor, yes (and everything is coherent: one container,
loaded, registered, shared with the sack). If the drone is destroyed + respawned from its own save
record (`key = n'drone'`), its BeginPlay — and hence the container spawn — happens **inside** the
main pass, i.e. AFTER the pre-pass, in which case `Array_Find(existingGatherersKeys,
'drone_InventoryContainer')` returns -1, `Array_Get(existingGatherers, -1)` zero-fills, the cast at
`[247]` fails, `loadData` is never called, and the container's saved `index` is silently lost.
`[?]` — I did not find a `gatherDataFromKey` or `skipPreDelete` override on `drone_C`
(`drone.hpp` lists `gatherDataFromKeyT` — the *trigger* variant — but not `gatherDataFromKey`),
which points at "respawned", but the `int_save` interface default was not measured.

> **Runtime probe that settles O-1:** at `loadObjects` completion, enumerate every live
> `prop_inventoryContainer_drone_C` and log `(pointer, propInventory.Index, propInventory.Owner)`,
> plus `gamemode.keyObj_key.Contains('drone_InventoryContainer')`, plus `drone.container` and
> `<sack>.container`. Three facts fall out at once: how many containers exist, whether they are the
> same object, and whether the index survived the load.

---

## 6. The sack lifecycle

**`drone::dropSack`** (94 statements, 3081 bytes) `[V]`:
```
[1] hasSack := false
[2] (sack)->SetVisibility(false, false)                     ; hide the drone's skeletal sack
[3] EX_LocalVirtualFunction delaySackInteraction            ; -> ubergraph 15625: sackInteract=false,
                                                            ;    Delay(1.0), then re-enable
[5] (Default__lib_C)->EX_LocalVirtualFunction "Prop To Object"(n'dronesack', self,
        out foodData, out object, out isFood, out propData)
[6] IsValidClass(object)
    -> spawn `object` deferred at coll.GetComponentLocation() - (0,0,100),
       rotation = K2_GetActorRotation(), collision-handling 2
    [24] if propData.parseNameToObject:
    [28]   (spawned as int_player)->asProp(out p)
    [29]   p.name := n'dronesack'
    [30]   p.init()
    [37] ((asProp)->StaticMesh)->SetPhysicsLinearVelocity((0,0,-250), false)   ; toss it down
    (fallbacks: isFood -> spawn prop_food_C + SetStructurePropertyByName(foodData) + name;
                else    -> spawn prop_C + SetNamePropertyByName(n'dronesack'))
```
So: **`Prop To Object` is the props-DataTable lookup** — it maps the row name `dronesack` to the
actor class + prop data, exactly as `propSpawner_editor::spawn` uses it `[V]`. It is not a
serialization step. The sack that appears in the world is a **freshly spawned physics prop**,
dropped with a downward impulse. `[V]`

**`drone::putSackOn(sack, velocityComponent)`** (15 statements) `[V]` — the reverse, when the
player brings the sack back to the drone (bound to `sackPhys`'s begin-overlap:
`BndEvt__drone_sackPhys_..._ComponentBeginOverlap` → `ExecuteUbergraph_drone(15592)` →
`15592: putSackOn(OtherActor, OtherComp)` `[V]`):
```
[0] jumpIfNot cond=hasSack        ; already carrying -> abort
[2] jumpIfNot cond=sackInteract   ; cooldown -> abort
[3] cast<prop_dronesack_C>(sack)
[6] (asDronesack)->takenByDrone := true         ; suppresses the respawn-on-destroy guard
[7-9] sackPhys.SetPhysicsLinearVelocity(velocityComponent.GetComponentVelocity() + (0,0,250), true)
[10] (sack)->K2_DestroyActor()
[11] hasSack := true
[12] (sack component)->SetVisibility(true, false)
```
Also called at BeginPlay-time recovery: `@15118` and, when `hasSack` is set but no
`prop_dronesack_C` exists in the world, the drone spawns one 1000 units above itself and
immediately `putSackOn`s it + plays `teleport_Cue` `[V]` (drone ubergraph `[515]`-`[531]`).

**The sack is self-healing.** `prop_dronesack_C::ReceiveDestroyed` `[V]`
(prop_dronesack ubergraph `[24]`-`[34]`):
```
[25] jumpIfNot cond=takenByDrone
[27]   lib.addHint("Dumbass stop destroying the drone sack")
[28-33] spawn a NEW prop_dronesack_C at gamemode.drone.GetActorLocation()
```
i.e. destroying the sack any way other than the drone taking it back respawns it.

**The sack's own wiring** `[V]` (prop_dronesack ubergraph):
```
BeginPlay:      [11] gamemode.getObjectFromKey(n'drone_InventoryContainer') -> container
trigger overlap:[19] actor := OtherActor ; [20] comp := OtherComp ; [21] trigger.SetCollisionEnabled(0)
                [22] Delay(0) -> [6] (container)->putObjectIn_overlap(actor, comp, Audio)
                                 [7] trigger.SetCollisionEnabled(1)
playerUsedOn:   [35] (container)->playerUsedOn(player, hit, lookAtComponent, holdObject, holdPropName)
action 4:       [39] (gamemode)->openPropInv(container)
getActionOptions: options_enum = [4]           ; the single "open" action
```
So the sack is a **remote handle to the same container**: dropping props onto it puts them into
`drone_InventoryContainer`, and opening it opens that container's UI. It holds nothing itself.
It has **no `propInventory` component of its own** — confirmed structurally: the SCS nodes in
`prop_dronesack.uasset` are only Audio / trigger / DefaultSceneRoot, and the `propInventory_C` /
`prop_inventoryContainer_drone_C` names appear in its import table purely because of the typed
`container` field and the `getObjectFromKey` cast `[V]`.

**Access to the delivered goods, both routes** `[V]`:
* via the drone: `actionOptionIndex` action == 4 → `(gamemode)->openPropInv(container)`
  (drone ubergraph `[536]`), gated on `canTakeOff` (`13229/15376: jumpIfNot cond=canTakeoff`);
  action == 7 → `dropSack()` (`[540]`).
* via the sack: as above.

---

## 7. Physics — why the sack does not dangle on the client

The visible dangling sack is **not** driven by the physics engine directly posing a mesh. It is an
AnimBP fed by a per-tick BP computation:

`ReceiveTick` → `ExecuteUbergraph_drone(1787)`, and `1787: EX_PushExecutionFlow 10358` — i.e. the
sack feed is the **first branch of the drone's Tick** `[V]` (CFG: `FunctionExport ReceiveTick`
→ `ExecuteUbergraph_drone(1787)`; block `@1787` pushes `10358`).

Block `@10358` `[V]`:
```
10376: sackPhys.K2_GetComponentLocation()          -> CallFunc_..._ReturnValue_1
10426: sackConstraint.K2_GetComponentLocation()    -> CallFunc_..._ReturnValue_2
10458: Subtract_VectorVector(_1, _2)               -> delta
10504: (droneSackAnim)->InstanceVariable[offset] := delta
10571: sackPhys.K2_GetComponentRotation()   -> BreakRotator -> Yaw
10667: sackConstraint.K2_GetComponentRotation() -> BreakRotator -> Yaw_1
10745: Subtract_FloatFloat(Yaw, Yaw_1)            -> dAngle
10791: (droneSackAnim)->InstanceVariable[angle] := dAngle
10840: EX_PopExecutionFlow
```
`droneSackAnim : UdronesackDraft_Skeleton_AnimBlueprint_C@0x04F0` is bound at BeginPlay from
`sack.GetAnimInstance()` `[V]` (drone ubergraph `[507]`-`[511]`).

**Therefore:** the coop mirror calls `D::SuppressTick(drone)` →
`E::SetActorTickEnabled(drone, false)` `[V]` (`ue_wrap/devices/drone.cpp:205`), which kills the
whole `ReceiveTick` ubergraph — including this block. `droneSackAnim.offset` and `.angle` freeze at
whatever they were, so the sack skeletal mesh is posed statically no matter what the `sackPhys`
sphere is doing. That is a complete, sufficient explanation of "the sack does not dangle at all on
the client". `[RD]` (the two halves — the tick feed and the tick suppression — are each `[V]`).

Secondary, same root: the client never runs `dropSack`/`putSackOn`, so `sack.SetVisibility(...)`
and `sackPhys.SetPhysicsLinearVelocity(...)` never fire on the mirror `[RD]`. The mod mirrors
`canTakeOff` and `hasSack` as raw field writes (`WriteGateFields`, `drone.cpp:305`) but **not** the
`sack` component's visibility `[V]` — so the mirror's sack mesh keeps its CDO/initial visibility
regardless of `hasSack`.

**Does a mirror need more than a pose stream?** Yes — a pose stream over the *actor* is
structurally insufficient here, because the thing the player sees is an AnimBP variable, not an
actor transform. Two families of answer exist (not designing, just naming them): stream
`offset`/`angle` (2 floats + a vector, the exact outputs), or leave the client's `sackPhys`
simulating locally and re-run only this Tick block on the mirror. `[RD]`

---

## 8. Dispatch-visibility table

Opcode legend: `EX_LocalVirtualFunction = 0x45` (BP-internal; **invisible** to a `ProcessEvent`
hook, catchable only via the mod's `GNatives[0x45]` swap — `ue_wrap/core/vm_dispatch.cpp:42`
`constexpr int kOpcodeLocalVirtual = 0x45;` `[V]`) · `EX_VirtualFunction`/`EX_FinalFunction`
(routed through `ProcessEvent` → **visible**) · `EX_CallMath` (direct native thunk, no
`ProcessEvent` → **invisible**).

All rows below read off the CFG dump (`kismet-analyzer gen-cfg`), which prints the literal opcode
and resolves `StackNode`/`VirtualFunctionName` through the import/export tables. No name-guessing.

| # | Call | Call site | Opcode as emitted | Visible to a PE hook? |
|---|---|---|---|---|
| 1 | `ui_laptop::makeAnOrder` | `daynightCycle::func_newHour [7]`, `trigger_eventer [331]/[354]`, `ui_laptop` ubergraph `[219]` | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 2 | `ui_laptop::addOrderCart` | `makeAnOrder [1]` | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 3 | `KismetArrayLibrary::Array_Add(saveSlot.orders, ...)` | `addOrderCart [0]` | `EX_FinalFunction` on `Default__KismetArrayLibrary` | native lib call, not an actor verb — useless as a seam `[V]` |
| 4 | `drone::sendShop` | `makeAnOrder [4]` and `[25]` | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 5 | `drone::beginFly` | drone ubergraph @13112, @13919 | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 6 | `drone::checkOrders` | drone ubergraph @10319, @13944 | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 7 | `drone::sendShop` (re-entry) | `checkOrders [6]` (@246) | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 8 | `uicomp_shopOrderSlot_C::upd` | `checkOrders [13]` | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 9 | `drone::soundAlarm` | drone ubergraph @12535 | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 10 | **`drone::compileOrder`** | drone ubergraph @12629 | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 11 | **`propInventory::addObject`** | `compileOrder [40]` (manyObj) and `[51]` (single) | `EX_LocalVirtualFunction`, Context = `(container)->propInventory` | **NO** (0x45) — this is the one the container lane hooks `[V]` |
| 12 | `int_coms::intComs_storeMiddleman` | `compileOrder [67]` | `EX_LocalVirtualFunction` on an interface context | **NO** (0x45) `[V]` |
| 13 | `int_player::sendName` | `compileOrder [86]/[99]/[109]` | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 14 | `AActor::K2_DestroyActor` (the consumed item) | `compileOrder [46]`, `[55]` | `EX_VirtualFunction` on a native target | **YES** `[V]` |
| 15 | `GameplayStatics::BeginDeferredActorSpawnFromClass` / `FinishSpawningActor` | `compileOrder [15]/[21]`, `[60]/[63]`; `dropSack`; drone BeginPlay @14206 | `EX_CallMath` | **NO** (direct native) — needs the SpawnActor/host_spawn_watcher path `[V]` |
| 16 | `ui_laptop::removeOrderCart` | drone ubergraph @12749 | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 17 | **`drone::dropSack`** | drone ubergraph @15361 (action 7) | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 18 | `drone::delaySackInteraction` | `dropSack [3]` | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 19 | `lib_C::"Prop To Object"` | `dropSack [5]` | `EX_LocalVirtualFunction` on `Default__lib_C` | **NO** (0x45) `[V]` |
| 20 | `prop_C::init` (on the fresh sack) | `dropSack [30]` | `EX_LocalVirtualFunction` | **NO** (0x45) — the classic `init()`-is-BP-internal trap `[V]` |
| 21 | `StaticMeshComponent::SetPhysicsLinearVelocity` | `dropSack [37]`, `putSackOn [9]` | `EX_VirtualFunction` on a native comp | **YES** `[V]` |
| 22 | **`drone::putSackOn`** | drone ubergraph @15118, @15592 (sackPhys overlap), @14... (BeginPlay recovery) | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 23 | `AActor::K2_DestroyActor` (the world sack) | `putSackOn [10]` | `EX_VirtualFunction` | **YES** `[V]` |
| 24 | `SceneComponent::SetVisibility` (drone's sack mesh) | `dropSack [2]`, `putSackOn [12]` | `EX_FinalFunction` on a native comp | **YES** `[V]` |
| 25 | `mainGamemode::getObjectFromKey` | drone BeginPlay @14071, sack BeginPlay `[11]` | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 26 | `mainGamemode::openPropInv` | drone ubergraph `[536]`, sack `[39]` | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 27 | `prop_container::putObjectIn_overlap` | sack ubergraph `[6]` | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 28 | `prop_container::playerUsedOn` | sack ubergraph `[35]`, drone `[547]` | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 29 | `propInventory::init` | `prop_container::spawned [4]` | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 30 | `lib_C::assignKey` | `prop_C::getKey [1]` | `EX_LocalVirtualFunction` | **NO** (0x45) `[V]` |
| 31 | `int_save::loadData` / `gatherDataFromKey` / `skipPreDelete` / `getKey` | `loadObjects [248]/[241]/[312]/[304]` | `EX_LocalVirtualFunction` on interface contexts | **NO** (0x45) `[V]` |

**Net:** essentially the entire order/delivery pipeline is 0x45. The only PE-visible events are
actor destroys, the two `SetPhysicsLinearVelocity` calls, and `SetVisibility`. Any lane on this
path must go through `vm_dispatch` (or a field poll, which the take-4 lesson
`lesson_netdelta_poll_aliases_and_lags_discrete_events` says loses fast spam and lags discrete
events).

---

## 9. What our mod syncs today, and the exact gap

### `coop/items/order_sync.cpp` — what it does

* **CLIENT:** polls `saveSlot.orders.Num` as a **watermark** (`OE::OrderCount()`), primes it on
  first tick so pre-existing orders are never forwarded (`g_forwardedThrough`), and on an
  *increment* serializes each new order (`price`, `size`, `category`, object class name — per item)
  into `OrderRequest` reliable chunks and forwards them to the host. It then calls
  `OE::QuietLocalDrone()` to reset the client drone's self-takeoff. It **never** mutates its own
  `orders` array. `[V]` (`order_sync.cpp:98-168`, and the file header comment says exactly this.)
* **HOST:** reassembles chunks per `(senderSlot, orderId)`, and commits a completed order through
  `OE::CommitOrder(od, automatic=true)` — i.e. the native `makeAnOrder(..., automatic=TRUE)`,
  which then queues + flies + drains natively. `[V]` (`order_sync.cpp:170-203`, `223-307`).

### What `order_sync` does **NOT** do

* It does not sync `saveSlot.orders` as state — only client→host *requests*. The host's queue is
  authoritative by construction; the client's own `orders` array is left diverged and unread
  (deliberately, and documented). `[V]`
* It does not touch the delivery at all: nothing about `compileOrder`, the container, the sack,
  `removeOrderCart`, or `dailyDelivery`. `[V]` (nothing in the file references them).
* `dailyDelivery` is a `saveSlot` bool with no lane — on a client running its own
  `daynightCycle`, `func_newHour` would fire its own automatic order. `[?]` whether the client's
  daynightCycle is parked by some other lane; not measured here.

### `coop/interactables/drone_sync.cpp` — what it does

Host-authoritative singleton pose stream: host reads `GetActorLocation/Rotation` at ~20 Hz while
`Active` and sends `DroneStatePayload`; client `SuppressTick`s the drone and interpolates the
transform onto it, plus an FX mirror (rotor dust replay, arrival cue, signal light) and
`WriteGateFields(canTakeOff, hasSack)` + `RepointContainer`. `[V]` (whole file).

### What `drone_sync` does **NOT** do

* No sack lane at all — not the world `prop_dronesack_C` (that rides the generic prop pipeline as
  an `Aprop_C`), not the `sack` component visibility, not `droneSackAnim.offset/angle`. `[V]`
* No `compileOrder`/`dropSack`/`putSackOn` interception. `[V]`
* `RepointContainer` uses `R::FindObjectByClass(L"prop_inventoryContainer_drone_C")` — **the first
  match**, with no key/identity check `[V]` (`drone.cpp:325`). If more than one such actor ever
  exists (see O-1), this points the mirror drone at an arbitrary one.

### `coop/props/container_contents_sync.cpp` — what it does

The GObjStack-slice lane, added this session. Host registers `addObject` + `takeObj` as 0x45
virtual verbs; `OnVerbEntry` resolves the eid of the component's `Owner` **at the edge** and adds
it to a dirty set; a 250 ms sweep re-resolves each eid forward, reads
`GObjStack[propInventory.Index].obj`, packs the `Fstruct_save` records and blob-chunks them under
`ReliableKind::ContainerContents`. Clients park unresolvable eids (30 s TTL) and apply by raw-
writing the TArray header, then re-derive `updateVolumesAndMass` + `recalculateNames`. Boundary 1
(`propInventory.Player == 0`, fail-closed) keeps personal inventories out; Boundary 2 neuters a
nested container's `ints[0][0]` to `-1`. `[V]` (whole file).

### The gap — why the delivery produced NO `container_contents` broadcast

The lane's send path has **five** serial preconditions, and each of the first four fails
*silently*:

```
compileOrder [40]/[51]  addObject   (0x45)  --> OnVerbEntry(br.ctx = the propInventory component)
  (1) IsHost() && session connected                          -> else return          [V] :453
  (2) IsInventoryComponent(br.ctx)                            -> else return          [V] :453
  (3) OwnerOf(br.ctx)  == propInventory.Owner @0xD0, non-null  -> else return          [V] :454
  (4) Registry::EidForActor(owner) != kInvalidId              -> else return          [V] :456-458
      => g_dirty.insert(eid)
  DrainDirty:
  (5) LivePropActor(eid) resolves && IsContainerActor && IsWorldContainerInventory     [V] :312-315
  (6) ReadContents(inv): GObjStackSlot(inv) non-null (needs Index >= 0 and
      Index < GObjStack.Num)  -- on failure BroadcastContainer returns TRUE
      ("nothing resolvable -- not a transport failure"), i.e. a SILENT no-op          [V] :268
```

The reported symptom — "the persistent `drone_InventoryContainer` eid stayed at 0 records through
the delivery" — is consistent with **(4)**, **(5)** or **(6)**, and this RE narrows it to a
specific mechanism:

**The most likely root `[RD]`: the eid that was seeded at connect and the actor `compileOrder`
wrote into are not the same actor** — because the drone always spawns its own container (§5),
while `container_contents_sync::QueueConnectBroadcastForSlot` seeds from
`Registry::SnapshotActorsByType(Prop)` filtered by `WalksToBase(prop_container_C)` `[V]`
(`container_contents_sync.cpp:531-543`). If the census enrolled a *different*
`prop_inventoryContainer_drone_C` (a save-loaded one, or the drone-spawned one under a different
eid after a churn), the seeded eid legitimately holds 0 records forever and the write lands on an
un-enrolled actor whose `EidForActor` returns `kInvalidId` at step (4) — no dirty mark, no
broadcast, no log line. **This is the "aimed at the wrong entity" failure in its exact shape.**

Explicitly ruled OUT as the cause:
* **Not** an `IsKeyedInteractable` miss. `Aprop_inventoryContainer_drone_C : Aprop_container_C :
  Aprop_C` `[V]`, and `IsKeyedInteractable` is `IsClassDescendantOfProp` ∪ {trashBitsPile,
  garbageClump, actorChipPile} `[V]` (`ue_wrap/actors/prop.cpp:143-155`). It is in the census
  universe. The same is true of the sack (`Aprop_dronesack_C : Aprop_C`) — **the sack is NOT
  excluded from the census by being save-excluded**; `prop_census::SeedWalk_` filters on
  `IsKeyedInteractable` + `IsLive` + not-CDO + not-hand-axis + not-child-actor `[V]`
  (`prop_census.cpp:92-112`), never on save-keyed-ness.
* **Not** a wrong verb. `addObject` at both `compileOrder` sites is emitted as
  `EX_LocalVirtualFunction` `[V]`, which is exactly the 0x45 the lane registers.
* **Not** the sack holding the goods. The sack has no inventory of its own `[V]` (§6).

Remaining candidates, in the order a probe should test them:
1. **(4)** `EidForActor(owner)` → `kInvalidId` because the drone-spawned container is not enrolled
   (spawned at BeginPlay, possibly before `StartCoopSession`, possibly a second actor). `[?]`
2. **(3)** `propInventory.Owner` null because `prop_container::spawned` (which calls
   `init(StaticMesh, self)`) did not run on this actor. `[?]`
3. **(6)** `Index` out of range because `loadData` never reached this actor (O-1). `[?]`

> **Runtime probe that discriminates all three in one shot:** in `OnVerbEntry`, before the early
> returns, log unconditionally — `ctx`, `ClassOf(ctx)`, `Owner`, `ClassOf(Owner)`,
> `propInventory.Index`, `propInventory.Player`, `EidForActor(Owner)`. One delivery then tells you
> which of (2)/(3)/(4) is falling out and whether the `Index` is sane. Pair it with the O-1 probe
> from §5 (enumerate every live `prop_inventoryContainer_drone_C` with its Index/Owner/eid).

---

## 10. What a sync lane must attach to

Not a design — just the anchor points this RE establishes, and the constraints on them.

* **Identity anchor = the container ACTOR the drone actually holds** (`drone.container@0x04F8`),
  not "the actor with key `drone_InventoryContainer`" and not "the first
  `prop_inventoryContainer_drone_C` found". Those three can differ (§5). Whatever the lane keys
  on, host and client must agree on the SAME actor, and the eid must be minted for the actor
  `compileOrder` writes into.
* **Contents anchor = `GObjStack[container.propInventory.Index].obj`** — one global array, indexed
  by a per-container slot number that is meaningless across peers (§1, §5). Any wire form must
  carry records, never the index.
* **Mutation seam = `propInventory::addObject` / `takeObj` at opcode 0x45** (row 11 of §8). There
  is no PE-visible alternative on this path.
* **Order-queue seam = `saveSlot.orders` + `ui_laptop::makeAnOrder`/`removeOrderCart`** — both
  0x45 (rows 1, 16). The queue is FIFO, popped by the laptop widget at delivery time, and both
  fire paths hand the drone `orders[0]`.
* **The sack is a handle, not a store.** Syncing the sack's *contents* is a category error; what
  needs syncing about the sack is (a) its existence/pose as a world prop, (b) the drone's
  `hasSack` + `sack` component visibility, (c) the AnimBP feed (§7).
* **Two host-authored RNG-ish/latch inputs sit upstream and are currently unsynced:**
  `saveSlot.dailyDelivery` (§2) and the conditional branches inside `Make Default Order`
  (`sendDriveBox` etc.). If a client's `daynightCycle` runs, it can queue its own daily order.
* **Mid-activity join (principle 8):** the delivery has at least four distinct states a joiner can
  land in — order queued but drone idle; drone in flight with `order` latched; arrived with items
  in the container and the sack on the drone; sack dropped in the world. Each needs an answer; the
  connect seed today covers only "container contents as of now" and the drone pose.

---

## 11. Open questions a design must close

| # | Question | Why static analysis cannot close it | Probe |
|---|---|---|---|
| O-1 | Is there ONE `prop_inventoryContainer_drone_C` at runtime, and did it get `loadData`? | Depends on drone BeginPlay vs `loadObjects` pre-pass ordering (§5) | enumerate live instances + `keyObj_key.Contains` at post-load |
| O-2 | Which of `OnVerbEntry`'s early returns fires during a delivery? | Needs live state | unconditional entry log (§9) |
| O-3 | Does the client's `daynightCycle::func_newHour` run and queue its own daily order? | Depends on which lanes park client world-sim | log `func_newHour` entry + `dailyDelivery` on both peers across a 06:00 boundary |
| O-4 | Is `sackPhys` actually simulating on the client (i.e. is the AnimBP feed the *only* break)? | CDO physics flags not read here | read `sackPhys->IsSimulatingPhysics()` on the mirror |
| O-5 | Does `int_save`'s default `gatherDataFromKey` return false (→ drone respawned by the loader)? | Interface default not measured | read the `int_save` BP defaults, or log drone BeginPlay vs `loadObjects` timestamps |
| O-6 | What do `prop_reelbox_C` / `prop_box_C` return from `intComs_storeMiddleman`, and is it RNG? | Not dumped this session | dump `prop_reelbox.uasset` `intComs_storeMiddleman` |

---

## Appendix — reproduction

```
KA=research/pak_re/tools/ka/kismet-analyzer-e8982e9-win-x64/kismet-analyzer.exe
C=research/pak_re/extracted/VotV/Content

$KA to-json  $C/objects/drone.uasset                        > drone.json
$KA gen-cfg  $C/objects/drone.uasset  ./cfg                 # -> cfg/drone.txt (byte offsets)
$KA to-json  $C/objects/prop_dronesack.uasset               > prop_dronesack.json
$KA to-json  $C/objects/prop_inventoryContainer_drone.uasset > pic_drone.json
$KA to-json  $C/objects/propInventory.uasset                > propInventory.json
$KA to-json  $C/objects/prop_container.uasset               > prop_container.json
$KA to-json  $C/objects/prop.uasset                         > prop.json
$KA to-json  $C/main/mainGamemode.uasset                    > mainGamemode.json
$KA to-json  $C/main/lib.uasset                             > lib.json
$KA to-json  $C/umg/interfaces/ui_laptop.uasset             > ui_laptop.json
$KA to-json  $C/objects/misc/daynightCycle.uasset           > daynightCycle.json
```
The statement-level renderer used above is a ~130-line python pretty-printer over the
`ScriptBytecode` arrays (resolving `PackageIndex` through Imports/Exports); it lives in the session
scratchpad and is trivially re-derivable. Grep evidence:
`grep -rl droneContainer $C` → `objects/drone.uasset` only;
`grep -rl keyObj_key $C` → `main/lib.uasset`, `main/mainGamemode.uasset`,
`objects/propSpawner_editor.uasset`;
`grep -rl makeAnOrder $C` → `objects/misc/daynightCycle.uasset`,
`objects/triggers/trigger_eventer.uasset`, `umg/interfaces/ui_laptop.uasset`;
`grep -rl sendShop $C` → `objects/drone.uasset`, `umg/interfaces/ui_laptop.uasset`.
