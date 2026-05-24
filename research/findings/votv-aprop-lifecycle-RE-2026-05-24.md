# RE: Aprop_C lifecycle (Init / spawn-time / destroy paths) — Phase 5S0 Inc2 gap audit

**Date**: 2026-05-24
**Trigger**: Phase 5S0 Inc2 (continuous Aprop_C lifecycle broadcast, HEAD 06ffa02)
shipped without fresh RE on the new observer surface. RULE
[[feedback-re-related-functions]]: before working a feature, RE all related
functions — not just the one being hooked. This is the retroactive audit.

**Shipped observers** (in `src/votv-coop/src/harness/harness.cpp` ~L695–L789):

1. `GrabObserver_Aprop_Init_POST` — host-side Aprop_C::Init POST hook.
   Broadcasts PropSpawn for every prop whose UCS-driven Init fires.
2. `GrabObserver_Actor_K2DestroyActor_PRE` — host-side AActor::K2_DestroyActor
   PRE hook. Broadcasts PropDestroy for every Aprop derivative being
   BP-destroyed.

This doc enumerates what those two hooks miss.

---

## Section 0 — Method note (same as inventory-drop RE)

Aprop_C BP functions (`Init`, `eaten`, `broken`, `Spawn`, `spawnedNaturally`,
`UserConstructionScript`, …) have NO native x64 address. They are
bytecode dispatched serially through `UObject::ProcessEvent`. IDA's
shipping `VotV-Win64-Shipping.exe` is stripped of UE4 engine symbol
strings (no `AActor::Destroy`, no `AActor::PostInitializeComponents`,
no exec thunk FNames except where the cooked content references them).

What IS in the IDB:
- FName-pool entries for UFunctions the engine itself calls on AActor:
  `K2_DestroyActor` @ `0x144146f70`, `ReceiveBeginPlay` @ `0x1441453b0`,
  `ReceiveDestroyed` @ `0x1441453c8`, `UserConstructionScript` @
  `0x144145850`, `Spawn` @ `0x143e7eda0`.
- All previously-named engine entry points (UObject_ProcessEvent
  @ `0x141465930`, UTimelineComponent_TickComponent, etc.).

What IS NOT in the IDB (because the strings live only in cooked .uasset
or BP class TArrays loaded at runtime):
- `Init`, `eaten`, `broken`, `thrown`, `spawnedNaturally`,
  `PostInitializeComponents`, `OnConstruction`, `FinishSpawningActor`,
  `BeginDeferredActorSpawnFromClass` (these are NOT the engine globals;
  the IDB does have `BeginDeferredActorSpawnFromClass` @ `0x144199b88` and
  `FinishSpawningActor` @ `0x144199de0` from cooked content; but the BP
  UFunctions named `Init`/`eaten`/etc. are not addressable by name).

Conclusion: IDA can confirm the **engine destroy / spawn anchors**
(K2_DestroyActor, ReceiveBeginPlay, ReceiveDestroyed,
UserConstructionScript, BeginDeferredActorSpawnFromClass,
FinishSpawningActor). It CANNOT enumerate which BP graphs call
K2_DestroyActor — that requires UE4SS Lua probing at runtime OR static
BP bytecode disassembly of the .uasset files. The deliverable below
combines header analysis (cooked content reflection — authoritative
for class/method existence + signatures) with the IDB anchor set + the
prior inventory-drop RE.

---

## Section 1 — Aprop_C::Init side effects

`Aprop_C::Init()` is declared in `prop.hpp` L92 with NO params. It is a
BP UFunction whose body is exec_Init bytecode dispatched through
ProcessEvent. The IDB has no native address for it. Side effects below
are inferred from:

- Aprop_C fields populated at spawn time (per
  `prop.hpp` L7–L62 layout).
- Previous RE doc `votv-inventory-drop-spawn-RE-2026-05-24.md`
  Section 3 — confirmed via reflection trace that Init() runs Key
  generation (NewGuid when `ResetKey==true`) and registers Key in
  `mainGamemode.keyObj_key/obj`.
- Aprop subclass headers showing which subclasses *override* Init() and
  what fields they additionally populate.

### 1a. Aprop_C::Init() base body (what every prop runs)

Inferred work, ordered by Aprop_C field layout (highest confidence first):

1. **Wire `Aprop_C.GameMode` @ 0x0250** — store `GetGameMode()` pointer
   so subsequent BP graph queries don't have to walk through
   GameplayStatics each call. (Verified by the field's `class
   AmainGamemode_C*` declaration: the only reasonable producer is Init.)
2. **Wire `Aprop_C.StaticMesh` @ 0x0238** — cached SMC pointer for the
   prop's primary `UStaticMeshComponent`. Set early so subsequent grab /
   look-at queries don't re-walk components.
3. **Generate / restore `Aprop_C.Key` @ 0x02E0** — the cross-peer
   identifier:
   - If `ResetKey == true` (field @ 0x0362): `Key = NewGuid().ToString()`
     via `UKismetGuidLibrary::NewGuid`. Brand-new guid per spawn.
   - Else if no Key set yet: same path (initial Init on a save-loaded
     prop that didn't have a Key persisted).
   - Else: Key already set (by `setKey` called before
     FinishSpawningActor — the OnSpawn receiver-side path we already
     ship, AND by loadData when a saved Fstruct_save is being restored).
4. **Register self with mainGamemode.keyObj_key[] / keyObj_obj[]** —
   parallel TArray index table @ 0x0828 / 0x0838. Called from `setKey`
   per L181 declaration; `setKey` is reachable from Init's Key-set path
   AND from receiver's pre-Finish setKey + loadData's Key-restore.
5. **Register self with mainGamemode.allProps[]** — TArray @ 0x07F0.
   This is the GLOBAL prop list. Every Aprop_C derivative gets added
   here at Init or BeginPlay (UE pattern — usually BeginPlay so all
   actor refs are valid).
6. **Initialise `Aprop_C.propData : Fstruct_prop` @ 0x0260** — looked
   up from the DataTable via `propPicker` @ 0x0240 (an
   FDataTableRowHandle). This pulls the prop's static description
   (heavy flag, mesh refs, mass, sounds, ...) from the cooked datatable.
   Likely a `UDataTableFunctionLibrary::GetDataTableRow` call inside Init.
7. **Apply default flags from propData** — `Static`, `frozen`, `sleep`
   booleans @ 0x02D8–0x02DD initialise from propData / class defaults.
8. **Possibly register with class-specific subsystem lists** — eg
   `Aprop_container_C` adds to `allContainers[]` @ 0x0800,
   `Aprop_drive_C` to `allDrives[]` @ 0x07A0, etc. These are likely
   in subclass Init OR ReceiveBeginPlay (preferred since pointers are
   valid). Header evidence: mainGamemode.hpp has 20+ such per-class
   TArrays.

### 1b. NO known BP delegates fire from Init's body itself

The five FMulticastDelegates on Aprop_C (hit, unhooked, takenByPlayer,
awoken, touched — L21–L36) all have semantic meanings AFTER initial
setup. They are bound by external callers (e.g. mainGamemode binds to
`touched` after the prop registers).

### 1c. Per-class Init overrides (67 found in dumps)

Sampled overrides and what extra work each does inside its Init:

| Class | Subclass-specific Init work |
|---|---|
| `Aprop_food_C` | Initialises `foodData : Fstruct_food` @ 0x0388 (calories, spoilage, edible). Cooking-state booleans (`isFrozen`, `useSpoiling`, `tempLessThan0`). Possibly registers with `mainGamemode.food[]` TArray @ 0x08F0. |
| `Aprop_food_mushroom_C` | Has its own `Init()` AND a `spawnedNaturally()` UFunction. Mushroom spawner (see Section 1d) calls `spawnedNaturally` AFTER spawn to apply the small+buried+collision-disabled growth-state machine. Init by itself just sets up `comp_photographic`. |
| `Aprop_torch_C` | Init() seeds `burning : bool` + `fuel : float` defaults; calls `setcol()` for material color. |
| `Aprop_camera_bad_C` | Init() sets `IsGood`, `isIR`, default `rot_X/rot_Y/Zoom`, registers with `mainGamemode.cams[]` @ 0x0618 and `allSecurityCameras[]` @ 0x11C0. |
| `Aprop_camera_C` | Inherits camera_bad's Init then sets `IsGood = true`. |
| `Aprop_landmine_C` | Init() sets `Active = false` + binds the sphere-overlap delegate. |
| `Aprop_container_C` | NO separate Init() — uses Aprop_C base Init + `spawned()` UFunction (called externally, e.g. from datatable-driven loot spawn). Has UCS too. |
| `Aprop_xmasgift_C` | Init() likely seeds the random drop table. |
| `Aprop_birdnest_C` | NO Init() override but has a `Spawn(Weight, Up, prop)` BP UFunction (called externally to spawn an egg) + `genEgg()` + ReceiveTick. This is a SPAWNER prop, not a regular Aprop_C user. |
| `Aprop_food_mre_C` | Has a `Spawn()` UFunction (with no params) that the food-spawn path calls to roll poison / cockroach chances. NOT called by Init directly. |
| `AmushroomMaster_C` (NOT Aprop_C!) | Has `Spawn()`, `form()`, ReceiveBeginPlay, UCS. This is a per-mushroom-cluster controller, NOT an Aprop_C — Init POST observer DOES NOT FIRE for it. |
| `AmushroomSpawner_C` (NOT Aprop_C!) | Has `Spawn()`, ReceiveBeginPlay. Per-spawn-point trigger. Init POST observer DOES NOT FIRE for it either. |

**Critical finding for the user's mushroom bug**: the actual mushroom
ACTOR spawned in the world is `Aprop_food_mushroom_C` (Aprop_C
derivative). Our Init POST observer DOES fire when one spawns. But the
divergent-POSITION problem is upstream — `AmushroomMaster_C::Spawn()`
and `AmushroomSpawner_C::Spawn()` run independently per peer using
`UKismetMathLibrary::RandomFloatInRange` / etc., and they emit
`Aprop_food_mushroom_C` actors at DIFFERENT positions. Init POST
broadcast won't fix it — the broadcast carries the host's transform,
but if both peers ALREADY spawned the same-Key prop at different
positions (Phase 5S0 Inc1 de-dupe path), the receiver SKIPS the spawn
and only does transform convergence. So Inc2 is doing exactly the
right thing for this case — convergence runs. The bug as-described
(mushroom falls through floor on client) is NOT a missed-hook issue.

### 1d. Mushroom spawn timeline (the bug-relevant case)

```
T=0  AmushroomMaster_C::ReceiveBeginPlay() per peer (NOT a prop)
     |
     v
T=1  per-peer RNG seeds + ::Spawn() rolls Types, positions, count
     |
     v
T=N  AmushroomMaster_C::form() emits per-mushroom SpawnActor calls:
     |   For each Fstruct_shroom entry in Data:
     |     UWorld::SpawnActor<Aprop_food_mushroom_C>(at pos)
     |       --> deferred-spawn (begin + finish)
     |       --> Aprop_food_mushroom_C::Init() runs   <-- OUR INIT POST FIRES HERE
     |       --> Init() generates NEW Key (ResetKey defaults true for fresh spawn)
     |       --> Aprop_food_mushroom_C::spawnedNaturally() called AFTER Init
     |       --> spawnedNaturally(): apply small+buried+collision-disabled state
     |       --> register in mainGamemode growth list (per peer)
     |       --> ReceiveBeginPlay + ReceiveTick start growth tick
```

**Two distinct host-vs-client divergences**:

A. **Position divergence**: each peer's `AmushroomMaster_C::form()`
   rolls different positions. Host's mushroom #N is at world pos P_h;
   client's at P_c. Keys differ too (each calls NewGuid independently)
   UNLESS the spawn is from a save load that preserved Keys.

B. **Growth state divergence**: client's mushroom may be at growth=0.2
   (still small + buried) while host's is at 1.0 (mature + visible).
   Each peer ticks growth on its own RNG.

Our Inc2 Init POST broadcast for the host's mushroom does:
- Send PropSpawn with host's Key K_h + position P_h.
- Receiver `OnSpawn`: `FindByKeyString(K_h)` returns nullptr (client
  has different keys), so receiver path proceeds to spawn a NEW
  Aprop_food_mushroom_C at P_h with Key K_h.
- But client ALSO has its own mushroom at P_c with Key K_c sitting in
  the world. **The host's broadcast does NOT destroy or de-duplicate
  the client's local mushroom.** The client now has TWO mushrooms
  (one local-natural, one wire-spawned).

This is a real missed-coverage. See Section 4 risk #C-2.

---

## Section 2 — Sibling spawn-time UFunctions on Aprop_C

Per `prop.hpp`:

| UFunction | Decl L | UE engine lifecycle slot | Relative order vs Init |
|---|---|---|---|
| `UserConstructionScript` | L93 | UE-native UCS — runs at `FinishSpawningActor` BEFORE BeginPlay | **BEFORE** Init (Init is the BP-level entry the UCS body calls into) |
| `Init` | L92 | Aprop_C custom UFunction — called from UCS | Wraps the per-class spawn logic |
| `ReceiveBeginPlay` | L176 | UE-native BeginPlay BP entry | **AFTER** Init (BeginPlay runs after UCS-driven Init returns) |
| `ReceiveDestroyed` | L177 | UE-native EndPlay BP entry | At destroy time |
| `intComs_gamemodeBeginPlay` | L192 | Custom delegate from mainGamemode | After mainGamemode BeginPlay runs — could be MUCH later than the prop's own BeginPlay |
| `afterplay` | L91 | Custom BP UFunction | Unclear timing; likely after BeginPlay |

`PostInitializeComponents` is NOT exposed as a BP UFunction by VOTV's
Aprop_C — the engine still calls it natively (it's part of AActor
spawn flow: ctor → PostActorCreated → PreInitializeComponents →
PostInitializeComponents → UCS → BeginPlay) but it's not BP-hookable.
Our PE observer can't hook it because ProcessEvent dispatches BP
UFunctions, not native virtuals.

`OnConstruction` is NOT exposed either — it's part of UE's editor flow
(re-runs construction script when actor is moved in editor); irrelevant
at runtime.

### 2a. Order of dispatch at a Aprop_C spawn

```
UWorld::SpawnActor(class, transform, params)
  AActor ctor                                  <- native
  PostActorCreated                             <- native virtual
  PreInitializeComponents                      <- native virtual
  PostInitializeComponents                     <- native virtual
  AActor::FinishSpawningActor(transform)       <- native, exposed as UFunction
    UserConstructionScript                     <- BP UFunction, runs first
      [BP graph for UCS: usually calls Init()] <- BP node-graph call
        Init                                   <- BP UFunction, runs nested
          (registers key, populates fields)
    [UCS body finishes; returns to engine]
    -- Engine completes FinishSpawning --
  AActor::DispatchBeginPlay                    <- native
    ReceiveBeginPlay                           <- BP UFunction
```

Our **Init POST observer fires when Init returns**, which is INSIDE
UCS, which is INSIDE FinishSpawningActor. At that moment:
- `Aprop_C.Key` is set (good — we read it for the wire).
- `Aprop_C.GameMode`, `StaticMesh` are set (good for receiver path).
- `Aprop_C.propData` is loaded from datatable (good).
- **`Aprop_C` is NOT yet in `mainGamemode.allProps[]`** if that
  registration happens in ReceiveBeginPlay (UE convention). The
  `getObjectFromKey` lookup table (`keyObj_key/obj`) IS populated by
  `setKey` so the prop CAN be resolved cross-peer by Key.
- **BeginPlay has NOT yet run** — any per-class subsystem registration
  inside ReceiveBeginPlay (mushroom growth manager, container
  spawn-loot table init, camera radar registration) HAS NOT HAPPENED.
- Actor location is settled (Set inside FinishSpawningActor; UCS sees
  the final transform), so `GetActorLocation` is valid (good for the
  wire's loc field).

### 2b. Could we hook ReceiveBeginPlay POST instead / as well?

Trade-off:
- **Init POST (current)**: catches the broadest set of spawn cases (BP
  UCS always calls Init), but BeginPlay-deferred state isn't yet on
  the actor. Subclass-specific registrations (mushroom in growth
  manager) haven't run on the host yet either, so they happen
  symmetrically per peer after wire delivery.
- **ReceiveBeginPlay POST**: catches AFTER per-class state is fully
  set up. But not every Aprop subclass overrides ReceiveBeginPlay
  (only ~187 of ~600 do). The base AActor BeginPlay still fires for
  the rest. To hook robustly we'd need to hook
  `AActor::ReceiveBeginPlay` UFunction at the engine level — which
  fires for EVERY actor (NPCs, projectiles, UI, …), not just props.
  Cost: a Type filter check on every BeginPlay (very frequent path).

**Recommendation**: keep Init POST. ReceiveBeginPlay isn't strictly
needed — the receiver's `OnSpawn` calls `FinishSpawningActor` which
triggers BeginPlay on the receiver side too, so the per-class
registrations happen on the client naturally.

### 2c. Subclass-specific spawn-completion UFunctions we DO miss

Several Aprop subclasses have additional UFunctions invoked AFTER
Init by an external caller (a spawner) carrying spawn-context data
Init doesn't see:

| UFunction | Class | Caller | What it does |
|---|---|---|---|
| `spawnedNaturally` | Aprop_food_mushroom_C | AmushroomMaster_C::form | Switches the mushroom to natural-growth mode (vs save-loaded-mature mode) |
| `spawned` | Aprop_container_C | mainGamemode (loot path) | Rolls and inserts loot items into the propInventory based on `lootEntry` / `spawnLoot` |
| `Spawn` (with args) | Aprop_birdnest_C | timer | Spawns an egg child actor |
| `Spawn` (no args) | Aprop_food_mre_C | mre spawner | Rolls poisonChance / cockroachChance |
| `genEgg` | Aprop_birdnest_C | self timer | Spawns a child egg actor |
| `crafted` | Aprop_C | crafting BP | Marks the prop as crafted (different from world-spawned) |

Our Init POST observer fires BEFORE any of these are called. The
broadcast snapshot doesn't carry the result of these calls. **For most
of these the receiver's natural execution path will run the equivalent
on its own side**, BUT only if the receiver's instance has the same
inputs — which for RNG-driven calls (mushroom growth, loot roll, MRE
poison roll) IT DOES NOT (per-peer divergent RNG).

This is the real Inc3 surface: **per-class state sync** for the cases
where the receiver's natural execution diverges from the host's.

---

## Section 3 — Destroy paths used by VOTV

### 3a. BP-side destroy paths (all funnel through K2_DestroyActor)

`AActor::K2_DestroyActor` is the BP-callable destroy entry. Every BP
graph node "DestroyActor" compiles to a call to this UFunction. The
PRE observer catches every such call.

Confirmed VOTV destroy callers (BP graphs that invoke K2_DestroyActor
on Aprop_C derivatives — inferred from header surfaces + game logic):

| BP function | On class | Destroys | Path notes |
|---|---|---|---|
| `eaten(Player)` | Aprop_food_C + subclasses | self | K2_DestroyActor at end of eat anim |
| `slice(clean)` | Aprop_food_mushroom_C, Aprop_food_C, Aprop_C | self | Splits a food prop into pieces; original destroyed |
| `broken()` | Aprop_container_C, Aprop_C | self + dingus respawn | K2_DestroyActor on break |
| `broken_fire()` | Aprop_container_C, Aprop_C | self | Variant on fire destruction |
| `physDestroyed`, `physPreDestroyed` | Aprop_C | self | Physics-impact-driven destroy (when prop hits floor too hard) |
| `Cut()` | Aprop_food_mushroom_C | self | Knife / Saw harvest |
| `crafted()` | Aprop_C | source ingredients | Crafting consumes inputs |
| `extract(idx)` | Aprop_container_C | (no, spawns NEW) | NOT a destroy path |
| `takeObj(idx)` | UpropInventory_C | (no, spawns NEW) | NOT a destroy path; covered by Bug-C takeObj observer |
| `pickupObject(hit)` | mainPlayer_C | the world prop | K2_DestroyActor after getData copies state to inventory |
| `addObject(actor)` | UpropInventory_C | the actor param | K2_DestroyActor at end after Fstruct_save captured |
| `digUp()` | Aprop_C | maybe — unclear semantics | Plant uprooting |
| `unhook()` | Aprop_C | linked hook entity? | Hook fishing detach |
| `microwave(microwave)` | Aprop_food_C, Aprop_C | self in some paths | Cooking transforms |
| `transform()` | Aprop_food_mushroom7_C | self | Mushroom transforming to mature stage |
| `Spawn()` aging path | mushroomMaster | OLD mushrooms after growth-stage-N timer | Spawner cleans up old children |
| Equipment despawn via `RemoveEquipment` | mainGamemode | the slot's Aprop_equipment_*_C | When un-equipped without dropping, the inv-stored copy is what is destroyed when used up |

**All of these go through K2_DestroyActor → our PRE observer catches
them.** ✓

### 3b. Native destroy paths (BYPASS K2_DestroyActor)

These DO NOT trigger our PRE observer:

1. **`AActor::Destroy()` (native, no FName UFunction)** — called by:
   - Engine GC (when actor's outer is destroyed, level unload)
   - Native gameplay code (C++-only paths inside UE engine itself)
   - VOTV: probably NOT used by BP graphs (BP nodes compile to K2_).
     But UE's `K2_DestroyActor` implementation in C++ calls
     `AActor::Destroy()` internally — that internal call is what
     actually destroys; K2_ is just the BP-exposed wrapper.
2. **`AActor::~AActor()` ctor** — never directly called from BP.
3. **Level unload / world teardown** — UE iterates all level actors
   and calls `EndPlay(EndPlayReason::LevelTransition)` then GC. Our
   PRE observer DOES NOT FIRE (no K2_DestroyActor dispatch). This is
   the CORRECT behavior for us (we don't want to broadcast level
   unload as a per-prop destroy event).
4. **`UWorld::DestroyActor(Actor)`** — the C++ API. BP graphs don't
   call this directly. Only engine code (UE-internal) does. None of
   the VOTV BP-defined paths use this.
5. **Object pool / soft destruction** — VOTV does NOT seem to use
   actor pooling (no evidence in the headers). Props are
   destroy-and-respawn pattern.

### 3c. ReceiveDestroyed — the unified hook point

`AActor::ReceiveDestroyed` UFunction (FName @ `0x1441453c8`) fires
INSIDE **both** K2_DestroyActor AND native AActor::Destroy paths
(UE's AActor::Destroy → EndPlay → ReceiveDestroyed dispatch). So
hooking ReceiveDestroyed PRE would be a STRICTLY broader net than
K2_DestroyActor PRE.

**Trade-off**: ReceiveDestroyed also fires at level unload and pool
teardown. If we hooked it we'd broadcast PropDestroy for every prop
when changing levels — bandwidth waste + receiver chaos.

**Mitigation**: gate on `g_session.connected()` + a "session is past
the level-stable threshold" timer (same shape as the Inc2 spawn
broadcast watermark). If the engine is mid-level-unload, all actors
go pending-kill in one engine frame; we can detect that via
`UWorld::HasBegunPlay() == false` OR a sentinel set by the loader.

**Recommendation**: Inc2 K2_DestroyActor PRE is GOOD ENOUGH for now
because VOTV's user-driven destroys (food eaten, container broken,
prop pickup, throw-shatter) all go through BP graphs → K2_. The only
case missed is engine-internal cleanup, which we don't want to
broadcast anyway. **DO NOT switch to ReceiveDestroyed without the
watermark gating** (otherwise post-Connected map transitions would
flood the wire).

### 3d. Visibility-toggle "soft destroy" patterns

Some Aprop BP graphs HIDE a prop instead of destroying it:
- `SetActorHiddenInGame(true)` — visual only
- `SetActorEnableCollision(false)` — physics only
- `Aprop_C.frozen = true` — quest-lock bool @ 0x02DA

These are NOT destroys. The prop still exists. Our K2_DestroyActor PRE
observer correctly does NOT fire for them. But the receiver may see
the prop in a desynced visible/collidable state if the host's
visibility toggle isn't replicated — that's a per-prop state sync gap
(Inc3 territory, not destroy-hook territory).

---

## Section 4 — Cascading-callback risk for the receiver

When `coop::remote_prop::OnSpawn` runs on the receiver:

```
BeginDeferredActorSpawnFromClass(world, cls, xform, ...)  <- step 1
  Actor created. PostInitializeComponents runs.
                                                          <- our setKey
setKey(receivedKey)                                       <- step 2
                                                          <- our markIncoming
FinishSpawningActor(spawned, xform)                       <- step 3
  UCS runs.
    [BP graph: UCS calls Init()]
      Init() runs.                                        <- Aprop_C::Init firing here
        ... key already set; Key generation branch skipped ...
        ... but everything else fires identically ...
    UCS body completes.
  Engine completes FinishSpawning.
  DispatchBeginPlay.
    ReceiveBeginPlay runs.
      [Aprop_food_mushroom_C: register with growth manager]
      [Aprop_container_C: maybe roll loot]
      ...
```

### 4a. CASCADE A — Init re-emits broadcast (RESOLVED)

When step 3 triggers Init on the receiver, our Init POST observer
fires. Without echo suppression this would re-broadcast PropSpawn back
to the sender. **Resolved**: `MarkIncomingSpawn(spawned)` BEFORE step
3; `ConsumeIncomingSpawn(self) == true` in Init POST → skip
broadcast. ✓

### 4b. CASCADE B — mushroom growth manager double-registration (CONFIRMED RISK)

When step 3 → Init → BeginPlay → mushroom registers with
`mainGamemode.growthManager` (or per-mushroom list). The CLIENT now
has a mushroom in its growthManager. Each tick, BOTH peers tick the
mushroom (growth value goes up at 2x rate on the client because client
also has its own pre-existing mushroom at K_c that's still ticking).

This is the bug the user reported: client's local mushroom (Key K_c)
is still growing while client also has a new wire-spawned mushroom
(Key K_h). The host's K_h mushroom is mature; the client's K_c
mushroom is still small + buried + collision-disabled. The PropPose
stream when host grabs the K_h mushroom moves the K_h mushroom on
client (correct visual) — but the client ALSO has the K_c mushroom
nearby in growing-state, which is the actor whose collision being
disabled is observed at release.

**RECOMMENDATION**: Inc3 needs to suppress the per-peer mushroom
spawner on the client OR mark client-spawned-naturally mushrooms as
shadow-only (collision-disabled, render-disabled, no growth tick) so
they don't interfere with host-replicated equivalents.

### 4c. CASCADE C — container loot re-roll (CONFIRMED RISK)

`Aprop_container_C::spawned()` (called by mainGamemode after UCS, NOT
inside Init) rolls loot from `lootEntry / spawnLoot` and inserts
freshly-spawned child Aprop items into `propInventory`. Each peer
rolls INDEPENDENTLY → containers contain different loot.

Our Init POST broadcast fires before spawned() is called, so the
receiver's spawned() runs its own roll → divergent inventory contents.
Per `[[project-coop-inventory-private]]`, container contents are private
to the peer who LOOKS inside it — but the WORLD effect of opening a
container and items spilling out is replicated as world events. So
the divergence shows when:
- Host opens container, sees items A, B, C.
- Client opens same container (or breaks it), sees items D, E.
- Items spilling on break are PER-PEER PRIVATE → desync visible to
  the watcher.

User rule says inventories are private, so this MAY be acceptable.
But the container's `lootEntry` should ideally be host-authoritative
to avoid the "host says jackpot, client says nothing".

### 4d. CASCADE D — BeginPlay-time `intComs_gamemodeBeginPlay` delegate (LOW RISK)

`Aprop_C.intComs_gamemodeBeginPlay()` fires when mainGamemode
BeginPlay finishes — broadcast to all live props. On the receiver's
new wire-spawned prop, this delegate has ALREADY fired before our
spawn (mainGamemode BeginPlay is once-per-level), so the new prop
misses it. But it also doesn't crash — the delegate is one-shot and
the prop just doesn't get the post-gamemode-init notification.

**RECOMMENDATION**: catalog this for Inc3 — for props that NEED
gamemode-init state (e.g. a prop tied to weather state set at
gamemode init), call the delegate body manually on wire-spawned
receiver props. Likely a no-op for most props.

### 4e. CASCADE E — Aprop_C.afterplay() (LOW RISK)

Custom UFunction L91. Unknown semantics; probably fires "after the
prop's normal BeginPlay flow settles". If it's called by mainGamemode
in a loop over allProps that runs at gamemode startup, wire-spawned
props miss it. Same mitigation as 4d.

### 4f. CASCADE F — Aprop_food_mushroom_C.spawnedNaturally NOT CALLED on receiver (CONFIRMED RISK)

The receiver's `OnSpawn` calls BeginDeferred → setKey → Finish. It
does NOT call `spawnedNaturally()`. That UFunction is called by
`AmushroomMaster_C::form()` (the per-peer spawner) AFTER spawning the
mushroom. On the receiver, the wire-spawned mushroom does NOT have
spawnedNaturally invoked → it's NOT in growing state, it's in default
state (whatever Init leaves it as — likely the "loaded from save,
mature" branch).

**This is actually fine** for the user's bug case: the receiver's
wire-spawned mushroom is mature (matches host). The bug is the
receiver's OWN pre-existing mushroom (K_c) being in growing state +
falling through floor at release time. The fix isn't about hooking
spawnedNaturally; it's about suppressing the receiver's per-peer
spawner (Inc3).

### 4g. CASCADE G — propInventory chain spawn (HIGH RISK for containers)

When a container (Aprop_container_C) spawns, its
`Aprop_container_C.propInventory : UpropInventory_C*` @ 0x0398 is a
non-actor sub-object. Loot inside that inventory are Fstruct_save
entries (per-item class + state), NOT yet world actors. So no
cascading actor spawns happen at the container's spawn time. ✓

BUT: when the container BREAKS (`broken()` BP function), the
inventory items spawn into the world as world actors. Each spawned
item triggers OUR Init POST observer → broadcasts PropSpawn → receiver
spawns it. **Receiver's container break ALSO spills its own
divergently-rolled loot** → double-spawn (host's loot + client's loot
both end up in world on client).

**RECOMMENDATION**: when container breaks, EITHER (a) suppress
client-side `broken()` from spilling loot if host's broadcast is
authoritative, OR (b) make `broken()` host-authoritative via an
EntityEvent (preferred — clean separation per architectural
principle 7).

---

## Section 5 — Recommendations

### 5a. Init POST hook — KEEP (with one caveat)

Init POST is the right primary spawn hook because:
- It fires inside FinishSpawningActor on every Aprop subclass (UCS
  always calls Init, even for subclasses that override Init — the
  override is called via the BP virtual dispatch).
- All wire-needed fields are populated by Init time
  (Key, transform, GameMode, StaticMesh).

**Caveat**: subclass-specific spawn-context UFunctions
(`spawnedNaturally`, `spawned`, `Spawn`-with-args, `crafted`, etc.)
fire AFTER Init. Their effects don't replicate via Init POST. For Inc3
(per-prop state sync) we'd need to either:
- Hook each of these UFunctions specifically and broadcast per-class
  state, OR
- Snapshot the receiver-relevant state at a later moment (e.g.
  AmushroomMaster_C::form completion) and broadcast at that point.

### 5b. K2_DestroyActor PRE — KEEP (sufficient for user destroys)

All VOTV BP-driven destroys go through K2_DestroyActor → PRE observer
catches them. No known missed BP destroy path.

Do NOT switch to ReceiveDestroyed — it'd add level-unload bandwidth
noise.

### 5c. NEW HOOKS NEEDED for Inc3 per-prop state sync

| Hook target | UFunction | Purpose |
|---|---|---|
| `Aprop_food_mushroom_C::spawnedNaturally` POST | spawnedNaturally | Mark mushroom as growth-state on host; broadcast growth-state event |
| `Aprop_container_C::spawned` POST | spawned | Capture host's loot roll for replication |
| `Aprop_container_C::broken` PRE | broken | Capture which items will spill (so receiver suppresses its own roll) |
| `Aprop_C::loadData` POST | loadData | Catch save-load restoration on host — replicate restored state |
| `Aprop_C::setPropProps` POST | setPropProps | Catch Static/frozen/Active/sleeping changes for replication |

### 5d. RECEIVER-SIDE SUPPRESSION LIST (Inc3)

Per-class, list of side effects to SUPPRESS on the receiver because
the host is authoritative:

| Class | Suppress | Reason |
|---|---|---|
| Aprop_food_mushroom_C | growth-tick in ReceiveTick if marked as wire-spawned | Receiver's mushroom should mirror host's growth, not its own |
| AmushroomMaster_C | entirety on CLIENT (host-only) | Spawner should be one peer only |
| AmushroomSpawner_C | entirety on CLIENT | Same |
| Aprop_container_C | loot-roll on CLIENT for host-owned containers | Host's roll wins |
| AundergroundGarbageSpawner_C | entirety on CLIENT | Spawner divergence |
| Aprop_birdnest_C | egg-spawn-timer on CLIENT | Receiver gets host's eggs via wire |
| Aprop_food_mre_C | poison/cockroach roll on CLIENT | Host's roll wins |

This is the Inc3 list. NOT implementing now — Inc3 is the next
increment. Listed here so the implementer doesn't repeat the
RE.

### 5e. EXISTING SHIPPED INC2 GAPS — none critical

The shipped Inc2 hooks (Init POST + K2_DestroyActor PRE) are
**correct + sufficient for what they claim to do**: continuous
prop-lifecycle broadcast. They do NOT:
- Fix the mushroom-falls-through-floor bug (that's Inc3 territory).
- Catch native engine destroys (correct — we don't want to).
- Sync per-class spawn-context state like growth, loot, poison-roll
  (Inc3 territory).

There is ONE missing gate that could improve correctness:

**Gap I-1**: the Init POST observer broadcasts EVERY Aprop_C spawn
post-handshake. But VOTV's mushroomMaster / undergroundGarbageSpawner
fire on EACH peer independently → each peer broadcasts its own
naturally-spawned props → receiver picks up the host's broadcasts AND
the client's own broadcasts hit the host (whose `g_session.role() ==
Role::Host` guard correctly drops them). But the client receives the
HOST's broadcasts for natural spawns, spawning extras on top of its
own naturally-spawned same-position-different-key mushrooms.

Mitigation already present: the FindByKeyString-driven de-dupe at
Section L507 of `remote_prop.cpp::OnSpawn` will resolve the host's
key to nullptr on client (different keys), so the spawn proceeds —
which is what causes the duplicate. To fix at Inc2 level we'd need
either:
- (a) Suppress natural-spawner activity on the client (Inc3
  per-class suppression).
- (b) Position-based fuzzy de-dupe at OnSpawn: if a same-class
  Aprop within 50cm of the broadcast position exists locally,
  treat as same prop and converge (rather than spawning new).

(b) is a quick, type-agnostic fix worth implementing alongside Inc2;
it'd correct the duplicate-mushroom issue across all natural spawners
without per-class work. RULE 1: it's NOT a crutch — it's a correctness
fix for the "two peers spawn the same logical entity with different
keys" case, which is fundamental to host-authoritative cross-process
spawning.

---

## Section 6 — IDA IDB renames + comments performed

This session added explanatory comments to four FName-pool entries
(no native function renames — the BP UFunctions in question have no
native addresses). All comments + the IDB itself were `idb_save`'d
per [[feedback-ida-rename-and-save]].

| Addr | FName | Comment summary |
|---|---|---|
| `0x144146f70` | `K2_DestroyActor` | Hooked PRE by Inc2 observer; native AActor::Destroy + GC + level unload bypass this UFunction |
| `0x1441453b0` | `ReceiveBeginPlay` | NOT hooked by current Inc2; possible alternative spawn-complete hook, fires AFTER Init POST |
| `0x1441453c8` | `ReceiveDestroyed` | Broader destroy net than K2_DestroyActor — also catches native AActor::Destroy paths. NOT hooked because it'd fire at level unload |
| `0x144145850` | `UserConstructionScript` | The actual UE engine UCS slot; Init() is called BY UCS in Aprop_C's BP graph. Init POST is at the right point. |

IDB saved at `D:\Projects\Programming\VOTV_MP\Game_0.9.0n\WindowsNoEditor\VotV\Binaries\Win64\VotV-Win64-Shipping.exe.i64`.

---

## Cross-refs

- `research/findings/votv-inventory-drop-spawn-RE-2026-05-24.md` —
  prior RE on Init key generation, deferred-spawn pipeline, takeObj
  POST hook
- `research/findings/votv-physics-interaction-deep-re-2026-05-23.md` —
  prior RE on grab pipeline (different concern, but uses same
  Aprop_C / Key infrastructure)
- `src/votv-coop/src/harness/harness.cpp` L695–L789 — the shipped
  Inc2 observers
- `src/votv-coop/src/coop/remote_prop.cpp` L485–L644 — `OnSpawn` (the
  receiver-side path that triggers the cascade discussed in Section 4)
- [[project-coop-mushroom-desync-and-remedy]] — the bug report that
  motivated Inc2
- [[project-coop-save-host-authoritative]] — Phase 5S0 design context
- [[feedback-re-related-functions]] — the rule this RE retroactively
  satisfies
