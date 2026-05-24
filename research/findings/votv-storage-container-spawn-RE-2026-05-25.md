# RE: VOTV storage container spawn routes — Init POST coverage gap analysis

**Date**: 2026-05-25
**Trigger**: User reports physics props spawned from storage containers (suitcase, chest, drawer, locker, etc.) are NOT synced across coop peers. Investigation question: does `Aprop_C::Init POST` fire for container-spawned item paths, and if so why does sync fail?

---

## Section 0 — Method

Reflection-driven only. CXXHeaderDump is authoritative for class hierarchies and UFunction signatures. All VOTV BP spawn paths route through ProcessEvent-dispatched UFunctions — no native bodies to IDA-decompile. Cross-referenced with:
- `votv-aprop-lifecycle-RE-2026-05-24.md` — Init timing, cascade analysis
- `votv-inventory-drop-spawn-RE-2026-05-24.md` — takeObj pipeline + Key restore timing
- `harness.cpp` L817 (Init POST observer), L999 (takeObj POST observer), L1027 (host-skip gate)
- `remote_prop.cpp` L534 (OnSpawn receiver — setKey before FinishSpawning pattern)

---

## Section 1 — Storage container class taxonomy

### 1a. Aprop_container_C family (prop_container.hpp)

`Aprop_container_C : Aprop_C` is the single base for all openable prop containers. Item storage: `UpropInventory_C* propInventory @ 0x0398` — items exist as `Fstruct_save` entries (not world actors) until extracted.

Key fields: `lootEntry @ 0x03A0` (FDataTableRowHandle), `spawnLoot @ 0x03B0` (FName), `UArrowComponent* Spawn @ 0x0388` (spawn-point marker, NOT a SpawnActor call).

Key UFunctions:
- `spawned()` — loot roll; fills propInventory.items with Fstruct_save entries; NO world-actor spawns
- `extract(int32 Index)` — UI extract path; calls propInventory.takeObj
- `getObject(int32 Index, bool customLoc, FVector Loc, AActor*& OutputPin)` — programmatic extract-with-location; also calls takeObj
- `openContainer()` — opens the UI or triggers animation; no world-actor spawn
- `broken()` — destruction; ejects inventory items via getObject/takeObj; also K2_DestroyActor on self

All 27+ concrete subclasses (`suitcase`, `crate`, `desk`, `drawers`, `wardrobe`, `barrel`, `mailbox`, `backpack`, etc.) are trivial or near-trivial (add at most `UChildActorComponent obstacle @ 0x0430`). NONE defines its own `Init()` override. They all dispatch to the base `Aprop_C::Init`. The subclass-aware Init scan in `InstallPropLifecycleObserversIfReady()` (harness.cpp:1128) registers the observer on the base `Aprop_C::Init` UFunction; container instances reach that same UFunction pointer through BP virtual dispatch.

### 1b. Off-hierarchy "storage" actors

**Alocker_C : AActor** (`locker.hpp`)
Inherits `AActor` directly. NOT `Aprop_C`. Has no `Init()` UFunction. Items inside a locker are stored in a NESTED `Aprop_container_C` instance accessible via `asContainer(Aprop_container_C*& container)`. The locker itself is a door/facade actor — its `Open(bool opened)` just plays an animation. All item storage and extract paths go through the inner `Aprop_container_C` (covered by Section 2). `Alocker_death_C : Alocker_C` is a variant. `IsDescendantOfProp(locker) == false` — Init POST correctly does not fire for the locker itself.

**Aprop_safe_C : Aprop_C** (`prop_safe.hpp`)
Inherits `Aprop_C` directly (not `Aprop_container_C`). Uses a completely different storage mechanism: `TArray<FName> Objects @ 0x0380` — array of prop DATATABLE ROW NAMES, not `Fstruct_save`. Items do not pre-exist in any inventory. `opened()` BP body iterates `Objects`, calls `mainGamemode.spawnPropThroughGamemode(name, spawnBox_transform, 1, out)` for each entry. `spawnPropThroughGamemode` routes through the standard `SpawnActor` -> UCS -> `Aprop_C::Init` path. Init POST fires. At Init POST time, Key = NewGuid from Init (fresh prop, no saved Fstruct_save to restore). This NewGuid IS the correct final Key for this actor — there is no subsequent `loadData` call that would overwrite it. VERDICT: safe items are correctly broadcast by Init POST with the correct key. No fix needed for this path.

**Aprop_treasureChest_C : Aprop_C** (`prop_treasureChest.hpp`)
Inherits `Aprop_C` directly. Fields `UStaticMeshComponent* gold @ 0x03A8` and `UStaticMeshComponent* keyM @ 0x0390` are built-in SMC children of the chest actor, NOT separate `Aprop_C` actors. `Open()` unlocks the `UPhysicsConstraintComponent` joints — no `SpawnActor` call. Init POST does not fire on `Open()`. The gold/key are visual physics components, not player-pickupable props. No coop wire needed.

---

## Section 2 — Aprop_container_C item spawn routes

### 2a. World spawn loot roll (spawned() — items become Fstruct_save)

```
Container appears in world (level load or SpawnActor):
  Aprop_container_C::UserConstructionScript -> Aprop_C::Init()
    [INIT POST FIRES HERE — for the CONTAINER itself]
    Key = NewGuid or saved Key; propInventory.items = empty
  Aprop_container_C::ReceiveBeginPlay
  [Later] mainGamemode calls spawned() on each container:
    Aprop_container_C::spawned()
      -> UpropInventory_C::addLoot()
          Reads lootEntry; rolls RNG; appends Fstruct_save entries to propInventory.items[]
          NO SpawnActor calls — items are stored as data, NOT as world actors
```

`spawned()` does NOT produce world actors. Our Init POST observer fires for the container itself at world spawn. It does NOT fire for loot items because there are no loot world-actor spawns at this stage.

`spawned()` is called per-peer independently, using per-peer RNG. Host gets items [A,B,C]; client gets items [D,E,F]. This is the secondary loot-divergence problem (Section 4b).

### 2b. Player extract via UI (the primary "items appearing" path)

```
Player interacts: openContainer() -> UI shows (no world spawn)

Player takes item (the spawn event):
  Aprop_container_C::extract(int32 Index)
    -> UpropInventory_C::takeObj(Index, removeVol=true, &ret, &out_save, &out_actor)
        STEP 1: BeginDeferredActorSpawnFromClass(world, out_save.class_3, spawn_transform, ...)
                -> Uninitialized AActor* returned
        STEP 2: [takeObj BP body: optionally set instance properties — usually none here]
        STEP 3: FinishSpawningActor(actor, transform)
                  -> UserConstructionScript runs
                      -> Aprop_C::Init() fires
                          Key = NewGuid (ResetKey=true by default for fresh spawn)
                          GameMode, StaticMesh, propData populated
                  -> ReceiveBeginPlay runs
                  FinishSpawningActor RETURNS. Actor is live.
        STEP 4: [back inside takeObj BP body] loadData(out_save) called
                  -> Aprop_C::loadData:
                      Key = out_save.key_64_* (the SAVED cross-peer-stable UUID)
                      This OVERWRITES the NewGuid written by Init
        STEP 5: takeObj returns; out_actor = fully keyed actor
```

Init POST fires at STEP 3 (inside FinishSpawningActor). At that moment `Aprop_C.Key @ 0x02E0` = NewGuid. The broadcast at `harness.cpp:888` reads `GetKeyString(self)` = NewGuid. Receiver spawns the actor and keys it to NewGuid. Then STEP 4 runs on HOST, overwriting Key with the saved UUID. Host now has the actor under saved_Key; receiver has it under NewGuid_key. All subsequent PropPose/PropGrab/PropDestroy from host carry saved_Key. Receiver's lookup for saved_Key fails. The prop is tracking-lost after the initial spawn.

### 2c. Container broken -> items eject

`broken()` calls `getObject(idx, customLoc=true, Loc, OutputPin)` for each propInventory item — which routes through `takeObj` (identical spawn chain as 2b). Same key timing issue.

On CLIENT, when a container breaks (client observes the damage or receives an EntityEvent, which is not yet implemented for containers): client's own `broken()` fires -> client's propInventory has divergent loot [D,E,F] -> each goes through takeObj -> takeObj POST observer fires (CLIENT-only) -> client broadcasts PropSpawn for items [D,E,F] with wrong keys. HOST ALSO broadcasts PropSpawn for [A,B,C] via Init POST (wrong keys). Receiver gets double-spawn: its own items + host's items, all with mismatched keys. This is the Cascade G scenario from the lifecycle RE.

### 2d. getObject() programmatic path

Identical to 2b in all respects (routes through takeObj). Same timing issue.

---

## Section 3 — Observer coverage vs. key timing

### 3a. Init POST (harness.cpp:817) — fires for container extracts; reads WRONG KEY

At Init POST time (STEP 3 above): Key = NewGuid. Fires on HOST (client returns at line 865). Does NOT skip (NewGuid is non-empty, passes the empty-key check at line 889). Broadcasts PropSpawn with wrong key.

### 3b. takeObj POST (harness.cpp:999) — reads CORRECT KEY; gated CLIENT-only

At takeObj POST time (after STEP 4 + STEP 5): Key = saved UUID from loadData. Reads correct key via the Object out-param at line 1014. However: line 1027 explicitly skips HOST with comment "Aprop_C::Init observer handles host broadcasts." The Init observer fires with the wrong key; the takeObj observer has the correct key but is muzzled on HOST.

### 3c. Receiver OnSpawn (remote_prop.cpp:534)

When receiver gets a PropSpawn packet, it calls `setKey(wire_key)` BEFORE `FinishSpawningActor` (remote_prop.cpp:710-731), preventing Init from generating a NewGuid. This is the correct receiver-side design. But the wire key it receives is the wrong NewGuid from step 3a, so it correctly prevents NewGuid overwrite but uses the wrong key from the broadcast.

---

## Section 4 — Root cause + fix

### 4a. Root cause: host-skip gate on takeObj POST + Init fires before loadData

The Init POST observer fires at the right lifecycle point (EVERY Aprop spawn) but at the WRONG data point (before loadData restores the saved Key). The takeObj POST observer fires at the right data point (after loadData, correct key) but is muzzled on HOST.

Fix is surgical: remove the host-skip gate in takeObj POST, and suppress Init POST from broadcasting for actors spawned during a takeObj call (so they don't double-broadcast with the wrong key then the correct key).

### 4b. Recommended fix: g_takeObjInFlight flag + dual-role takeObj POST

Three changes to `src/votv-coop/src/harness/harness.cpp`:

**(1) NEW: GrabObserver_PropInventory_TakeObj_PRE** — new PRE observer on the same `propInventory_C::takeObj` UFunction (registered alongside the existing POST in `InstallInventoryObserverIfReady`):

```cpp
// PRE observer: signal that Init POST must defer broadcast for the actor
// that is about to be spawned inside this takeObj call.
// Game-thread-only; single-threaded BP dispatch — no atomic needed.
static bool g_takeObjInFlight = false;

void GrabObserver_PropInventory_TakeObj_PRE(void* self, void* function, void* /*params*/) {
    if (!self || !g_session.connected()) return;
    g_takeObjInFlight = true;
}
```

**(2) MODIFIED: Init POST** — before the broadcast block (after the g_processedInitActors and WireSuppressed checks), add:

```cpp
// If this actor is being spawned from inside a takeObj call, defer broadcast.
// takeObj POST fires after loadData and will broadcast with the correct saved Key.
// Without this guard, we would broadcast NewGuid here and takeObj POST would
// broadcast the saved UUID -- receiver gets duplicate PropSpawn with two different
// keys for the same actor (NewGuid first, then correct Key), causing duplicate spawns.
if (g_takeObjInFlight) {
    UE_LOGI("grab_hook[Aprop.Init POST]: actor %p spawned inside takeObj -- deferred to takeObj POST (key not yet restored by loadData)",
            self);
    return;
}
```

**(3) MODIFIED: takeObj POST** — remove the host-skip gate (lines 1027-1030); let both host and client broadcast via takeObj POST for container extract spawns. Add g_takeObjInFlight clear at the top of the observer:

```cpp
// Clear the in-flight flag regardless of further filtering --
// the flag must always be reset when takeObj POST fires.
g_takeObjInFlight = false;
```

Remove:
```cpp
// REMOVE this block:
if (g_session.role() != coop::net::Role::Client) {
    UE_LOGI("grab_hook[takeObj POST]: host-side drop -- skip (Aprop_C::Init observer handles host broadcasts)");
    return;
}
```

**PRE observer registration** in `InstallInventoryObserverIfReady()` alongside the existing POST.

**RULE 2 compliance**: Init POST remains the primary path for non-takeObj spawns (mushrooms, world-gen, spawnPropThroughGamemode, save-load filtered by !connected). takeObj POST becomes the canonical path for all extract spawns on both host and client. No old path is kept "for compatibility."

**Re-entrancy note**: `g_takeObjInFlight` is a single bool. If takeObj were to call itself re-entrantly (extremely unlikely — would require a BP in a container's ReceiveBeginPlay that immediately extracts another container), the inner PRE sets the flag true, inner POST clears it, then the outer takeObj POST would see the flag already false. The outer Init POST (for the outer takeObj's spawned actor) would have been suppressed correctly by the outer PRE. Net effect: in re-entrant case, inner item broadcasts correctly; outer item also broadcasts correctly. The only risk is if the outer actor's Init POST fires BEFORE the inner PRE fires (impossible — single threaded, linear dispatch order). Safe as-is.

### 4c. Secondary fix (loot divergence — Inc3 scope, not required for immediate bug)

`spawned()` is called per-peer independently -> divergent propInventory contents. Fix requires:
- Hook `Aprop_container_C::spawned` POST on HOST.
- Send ContainerLootPacket {containerKey, TArray<Fstruct_save> items} to client.
- Receiver: match container by Key -> overwrite propInventory.items with host's list.

This is clearly Inc3 (per-prop state sync) scope. Not needed for the "items don't appear" symptom. Fix 4b alone ensures that when host extracts an item, client receives it with the correct key and can track it via PropPose.

**Container broken double-spawn** (Cascade G): when a container breaks, client's divergent loot also ejects. This requires host-authoritative `broken()` — either via EntityEvent suppressing client-side broken(), or via explicit "host is the only peer that ejects loot on break" rule. Also Inc3 scope.

---

## Section 5 — Verification plan

1. **Key timing probe** (runtime): temporary harness log at Init POST (log Key before the new g_takeObjInFlight check) and at takeObj POST (log Key after the flag clear). Expected sequence for a container extract:
   - takeObj PRE fires: g_takeObjInFlight = true
   - Init POST fires: Key = some-guid-string; "deferred to takeObj POST" log
   - takeObj POST fires: Key = stable-UUID-string; broadcasts with correct key

2. **No double-broadcast**: PropSpawn log should show exactly ONE packet per extract, carrying the saved UUID.

3. **Echo suppression**: OnSpawn on the receiver side marks the actor with `MarkIncomingSpawn` before FinishSpawningActor. When Init POST fires on RECEIVER for that wire-received actor, `ConsumeIncomingSpawn` returns true -> skip (harness.cpp:825). Removing the host-skip gate does not break this.

4. **PropPose resolution after extract**: log that PropPose packets arrive with the same key as the PropSpawn. Receiver's `FindByKeyString` should resolve correctly.

---

## Section 6 — File reference

Files to modify for Fix 4b:
- `src/votv-coop/src/harness/harness.cpp`
  - L817: `GrabObserver_Aprop_Init_POST` — add g_takeObjInFlight early return before the broadcast block
  - L999: `GrabObserver_PropInventory_TakeObj_POST` — add flag clear at top; remove host-skip gate at L1027-1030
  - L1202: `InstallInventoryObserverIfReady` — register new PRE observer on the same `takeObj` fn ptr
  - Add: `static bool g_takeObjInFlight = false;` near L1124 (alongside other lifecycle state)
  - Add: `GrabObserver_PropInventory_TakeObj_PRE` function body near L999

No changes needed to:
- `remote_prop.cpp` (OnSpawn + echo suppression already correct)
- `sdk_profile.h` (PropInventoryTakeObjFn already defined at L631; reused for PRE)
- Any container class headers (read-only RE)

Key offsets:
- `Aprop_container_C.propInventory @ 0x0398` — UpropInventory_C* (item store)
- `Aprop_container_C.lootEntry @ 0x03A0` — FDataTableRowHandle (which loot table)
- `Aprop_container_C.spawnLoot @ 0x03B0` — FName (which row to roll)
- `Aprop_safe_C.spawnBox @ 0x0378` — UBoxComponent* (spawn location)
- `Aprop_safe_C.Objects @ 0x0380` — TArray<FName> (datatable row names)
- `Aprop_C.Key @ 0x02E0` — FName (written as NewGuid by Init; overwritten by loadData)
- `Aprop_C.ResetKey @ 0x0362` — bool (true by default -> Init generates NewGuid)
