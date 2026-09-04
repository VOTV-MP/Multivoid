# Wiring chipPile / garbageClump / trashBitsPile into the existing Aprop_C pipeline — 2026-05-27

User directive (verbatim):
> we already have synced object interactions, we just never wired chipPile

Goal: identify the minimal change to make `AactorChipPile_C`,
`Aprop_garbageClump_C`, `AtrashBitsPile_C` (and the 6 subclass variants)
ride the existing `PropPose` / `PropRelease` / `PropSpawn` / `PropDestroy`
pipeline — and retire the parallel STAGE 2 `non_prop_entity_sync` stack
per RULE №2.

---

## 1. Key storage per class (offsets from CXX dump)

Read of:
- `Game_0.9.0n/.../CXXHeaderDump/actorChipPile.hpp` (+ erie / leaves / wetConcrete)
- `Game_0.9.0n/.../CXXHeaderDump/prop_garbageClump.hpp` (+ erie / leaves / wetConcrete)
- `Game_0.9.0n/.../CXXHeaderDump/trashBitsPile.hpp`
- `Game_0.9.0n/.../CXXHeaderDump/actor_save.hpp`
- `Game_0.9.0n/.../CXXHeaderDump/prop.hpp` (baseline)

| Class | Parent | Size | Key (FName) offset | setKey UFunction | GetKey UFunction |
|---|---|---|---|---|---|
| `Aprop_C` | `AActor` | — | **0x02E0** (visible field) | yes | yes |
| `AactorChipPile_C` | `AActor` | 0x274 | **NO visible field** (BP-VM-internal) | yes | yes |
| `AactorChipPile_erie_C` | `AactorChipPile_C` | 0x2A4 | inherited (BP-VM) | inherited | inherited |
| `AactorChipPile_leaves_C` | `AactorChipPile_C` | 0x274 | inherited | inherited | inherited |
| `AactorChipPile_wetConcrete_C` | `AactorChipPile_C` | 0x284 | inherited | inherited | inherited |
| `Aprop_garbageClump_C` | `AActor` | 0x2F4 | **NO visible field** (BP-VM-internal) | yes | yes |
| `Aprop_garbageClump_erie_C` | `Aprop_garbageClump_C` | 0x300 | inherited (BP-VM) | inherited | inherited |
| `Aprop_garbageClump_leaves_C` | `Aprop_garbageClump_C` | 0x2F4 | inherited | inherited | inherited |
| `Aprop_garbageClump_wetConcrete_C` | `Aprop_garbageClump_C` | 0x2F8 | inherited | inherited | inherited |
| `AtrashBitsPile_C` | `Aactor_save_C` | 0x274 | **0x0230** (inherited from Aactor_save_C) | yes | yes |

**Conclusion**: there are **THREE different Key storage layouts** here:
- Aprop_C lineage → `0x02E0` direct field
- Aactor_save_C lineage (trashBitsPile) → `0x0230` direct field
- AActor-direct lineage (chipPile, clump) → **no visible field**; Key is BP-VM-only

For chipPile + clump there is no stable native offset; their `setKey` /
`GetKey` UFunctions must be dispatched via ProcessEvent. This is RULE-1
compliant — `GetKey()` is the canonical BP-author-defined read accessor.

Important behavioural note from `votv-chippile-clump-morph-RE-2026-05-27.md`:
chipPile and clump are **TRANSIENT** entities that morph at pickup/throw
(chipPile-A → clump → chipPile-B; three distinct UObjects). They are not
save-Key-stable — `clump.skipSave1 @0x0254` exists, the clump is
not normally persisted; the chipPile is save-persisted but its Key (if any)
is BP-internal and likely re-minted on each Init via NewGuid.

---

## 2. Existing Aprop_C pipeline call sites (gating checks)

Searched `src/votv-coop/` for `IsClassDescendantOfProp` / `IsDescendantOfProp`:

| File:Line | Caller | Gate purpose |
|---|---|---|
| `ue_wrap/prop.cpp:45` | `IsDescendantOfProp(obj)` | wrapper: ClassOf then IsClassDescendantOfProp |
| `ue_wrap/prop.cpp:51` | `IsClassDescendantOfProp(cls)` | SuperStruct chain walk (≤16 hops) vs prop_C base |
| `ue_wrap/prop.cpp:119` | `FindNearest()` GUObjectArray walk | candidate filter |
| `ue_wrap/prop.cpp:255` | `FindNearbySameClass()` | candidate filter (Gap I-1 fuzzy) |
| `ue_wrap/prop.cpp:287` | `FindByKeyString()` | candidate filter (wire receive) |
| `coop/prop_snapshot.cpp:64` | `Trigger()` enumeration | snapshot-on-connect filter |
| `coop/prop_lifecycle.cpp:169` | `GrabObserver_Aprop_Init_POST` | broadcast gate after Init |
| `coop/prop_lifecycle.cpp:290` | `GrabObserver_Actor_K2DestroyActor_PRE` | broadcast gate for destroy |
| `coop/prop_lifecycle.cpp:346` | `GrabObserver_PropInventory_TakeObj_POST` | extracted-actor must be prop_C |
| `coop/prop_lifecycle.cpp:467` | `Install()` Init scan | walk UFunctions whose Outer is prop_C lineage |

The standard Aprop_C path is therefore activated by:
1. **Init POST observer** — registered via subclass-aware Init scan that uses
   `IsClassDescendantOfProp(owningCls)` to pick which Init UFunctions to hook.
2. **K2_DestroyActor PRE observer** — global engine-level UFunction; per-actor
   gate is `IsDescendantOfProp(self)`.
3. **PropPose per-tick stream** — driven from `harness.cpp:469` reading
   `mainPlayer.grabbing_actor @ 0x07D0` and calling
   `prop_wrap::GetKeyString(heldActor)` which reads `Aprop_C.Key @ 0x02E0`.
4. **PropRelease one-shot** — emitted at the held → not-held edge in
   `harness.cpp:515-528`.
5. **Snapshot on connect** — `coop/prop_snapshot.cpp:64` walks GUObjectArray
   filtering on `IsDescendantOfProp`.
6. **Grab observers** (`coop/grab_observer.cpp`) — hook
   `UPhysicsHandleComponent`, `UPhysicsConstraintComponent`,
   `UPrimitiveComponent::AddImpulse`, and 3 mainPlayer Timeline funcs.
   These are **engine-level** UFunctions; they fire regardless of grabbed
   class. **NO gating on prop_C** in this file.

---

## 3. Does the BP-level grab mechanism cover chipPile/clump?

**For `AtrashBitsPile_C`**: same shape as chipPile (uses `playerTryToHold`,
`playerHandRelease_*`, no PHC). Same answer below.

**For chipPile + clump: NO.** This is the load-bearing finding.

The chipPile/clump pickup mechanism (per
`votv-chippile-clump-morph-RE-2026-05-27.md` §1.1 + §1.2 + §4):

```
mainPlayer trace -> AactorChipPile detected
  -> chipPile.playerTryToHold(Player, out collected)
     -> internally: chipPile.toClump()
        -> spawn Aprop_garbageClump_C at chipPile's transform
        -> set clump.holdPlayer = Player
        -> set clump.canConvert = true
        -> chipPile.K2_DestroyActor()
  -> mainPlayer.holding_actor @ 0x0A20 = clump  (NOT grabbing_actor!)
```

Critical distinctions vs the Aprop_C grab pipeline:

1. **No UPhysicsHandleComponent.GrabComponentAtLocation call** — the
   grab_observer's PHC observers DO NOT FIRE.
2. **No UPhysicsConstraintComponent.SetConstrainedComponents call** —
   the PCC heavy-grab observer DOES NOT FIRE.
3. **mainPlayer.grabbing_actor stays NULL.** The harness's PropPose emitter
   at `harness.cpp:468-469` reads `MainPlayer_grabbing_actor()` and is
   therefore **silent for chipPile/clump pickups**. No PropPose ever goes
   on the wire. No PropRelease either (the harness's held→not-held edge
   tracks `g_lastHeldProp` which was set from `grabbing_actor`).
4. The held-pose drive on the host is **either** clump.ReceiveTick
   reading `holdPlayer` and updating its transform per-tick, **or**
   mainPlayer.holding_actor → SetActorLocation from outside, **or** a
   one-shot K2_AttachToActor at toClump() time. Per the morph RE (§4.4)
   the strongest evidence is attach-to-actor; either way, **NEITHER drives
   `grabbing_actor`**.
5. **The throw path also differs**: `playerHandRelease_LMB` on the clump
   does NOT call `UPhysicsHandleComponent.ReleaseComponent`; it calls
   `SetPhysicsLinearVelocity` directly via `throwHoldingProp`. The
   `PrimComp_AddImpulse` observer might catch this BUT only if the BP
   uses AddImpulse rather than SetPhysicsLinearVelocity, and we don't
   currently link AddImpulse to a PropRelease emit (it's diagnostic-only).

**So the user's premise — "PropPose / PropRelease cover them" — DOES NOT
HOLD for chipPile and clump** as the pipeline stands. Wiring them in
requires either:

- **(A)** Switch the harness's PropPose emit to ALSO check
  `mainPlayer.holding_actor @ 0x0A20` (a second held-actor slot), and
  emit PropPose/PropRelease for whichever is non-null. This piggybacks on
  the existing wire packets — no new packet kinds. This is the user's
  intended minimal change.
- **(B)** Add the host-tick clump-pose stream as a new observer-driven
  path (e.g. POST observer on `Aprop_garbageClump_C::ReceiveTick`). Cost:
  per-tick observer fan-out on every garbage clump in the world ever.
  Heavier.

Option **(A)** is the minimal-code path and is what the user is pointing at.
It piggybacks on the **same PropPose key-routing the receiver already has**
because `remote_prop::Tick` resolves the wire key via
`prop_wrap::FindByKeyString` (`remote_prop.cpp:230`) — which does
`IsDescendantOfProp` filter today. Once we relax that filter (see §4) the
receive side is unchanged.

**For `AtrashBitsPile_C`**: same pickup mechanism, same path. trashBitsPile
also relies on `playerTryToHold` + a morph (it "collects into the inventory"
rather than spawning a clump in hand — see trashBitsPile.hpp line 30
`playerTryToHold`; this needs a runtime probe to confirm, but it's not a
PHC grab either).

---

## 4. The minimal code change — proposal

### 4.1 Single new helper: `IsKeyedInteractable`

Add a parallel `IsKeyedInteractable(void* cls)` next to
`IsClassDescendantOfProp` in `src/votv-coop/include/ue_wrap/prop.h` +
`prop.cpp`. This is the cleaner RULE-1 path than overloading
`IsClassDescendantOfProp` (which has a documented contract — "derives
from prop_C base"); overloading would mean every caller of
IsDescendantOfProp (which is also used for collision-restore checks,
heavy/static reads at `Aprop_propData_heavy` offset etc.) gets non-Aprop_C
objects fed in and reads the wrong offsets.

```cpp
// ue_wrap/prop.h
// True if `cls` is (a) prop_C or a derivative, OR (b) one of the keyed-interactable
// non-Aprop_C lineages (chipPile / garbageClump / trashBitsPile / their subclasses).
// Used by the wire-route gates that only care about "can this actor participate
// in the keyed PropPose/PropRelease/PropSpawn/PropDestroy pipeline", regardless
// of whether the Key storage is the Aprop_C +0x2E0 field, the Aactor_save_C
// +0x230 field, or a BP-VM-only field reachable via GetKey() UFunction dispatch.
bool IsKeyedInteractable(void* cls);
```

Implementation: walk the SuperStruct chain once; return true if any hop is
prop_C base, OR matches the cached non-Aprop UClass set. The non-Aprop set
is initialised lazily once at first hit:

```cpp
// ue_wrap/prop.cpp
namespace {
struct NonAPropBaseSet {
    void* chipPile = nullptr;
    void* clump = nullptr;
    void* trashBitsPile = nullptr;
    bool resolved = false;
};
NonAPropBaseSet g_nonApropBases;

void TryResolveNonAPropBases() {
    if (g_nonApropBases.resolved && R::IsLive(g_nonApropBases.chipPile)) return;
    g_nonApropBases = {};
    g_nonApropBases.chipPile      = R::FindClass(L"actorChipPile_C");
    g_nonApropBases.clump         = R::FindClass(L"prop_garbageClump_C");
    g_nonApropBases.trashBitsPile = R::FindClass(L"trashBitsPile_C");
    g_nonApropBases.resolved = g_nonApropBases.chipPile &&
                               g_nonApropBases.clump &&
                               g_nonApropBases.trashBitsPile;
}
}  // namespace

bool IsKeyedInteractable(void* cls) {
    if (!cls) return false;
    if (IsClassDescendantOfProp(cls)) return true;
    TryResolveNonAPropBases();
    if (!g_nonApropBases.resolved) return false;
    // SuperStruct walk against the 3 non-Aprop bases — catches subclasses too.
    for (int hops = 0; hops < 16 && cls; ++hops) {
        if (cls == g_nonApropBases.chipPile ||
            cls == g_nonApropBases.clump ||
            cls == g_nonApropBases.trashBitsPile) return true;
        cls = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(cls) + P::off::UStruct_SuperStruct);
    }
    return false;
}
```

### 4.2 Key reader: per-lineage offset + BP-UFunction fallback

`prop_wrap::GetKey(actor)` currently reads `*(actor + 0x02E0)` (Aprop_C's
Key offset). For chipPile / clump that offset points at unrelated data
(`pile`, `clump`, `flameBase`, etc. — clearly NOT a Key). RULE-1 fix:

```cpp
// ue_wrap/prop.cpp
R::FName GetKey(void* prop) {
    if (!prop) return R::FName{0, 0};
    void* cls = R::ClassOf(prop);
    if (IsClassDescendantOfProp(cls)) {
        return ReadField<R::FName>(prop, P::off::Aprop_Key);   // +0x02E0
    }
    TryResolveNonAPropBases();
    // trashBitsPile: inherits Key from Aactor_save_C @ +0x0230 (direct field)
    if (cls == g_nonApropBases.trashBitsPile ||
        IsSubclassOf(cls, g_nonApropBases.trashBitsPile)) {
        return ReadField<R::FName>(prop, 0x0230);
    }
    // chipPile / clump (and their subclasses): no native field; dispatch GetKey().
    if (IsSubclassOf(cls, g_nonApropBases.chipPile) ||
        IsSubclassOf(cls, g_nonApropBases.clump)) {
        return GetKeyViaUFunction(prop);   // ProcessEvent dispatch
    }
    return R::FName{0, 0};
}
```

`GetKeyViaUFunction` resolves `GetKey(FName& Key)` on the actor's class
once (cached per-class), prepares a frame with the out-param slot, calls
via R::CallFunction, returns the out FName. Same shape as
`prop::GetPhysicsVelocity` (already in prop.cpp:212-242). Per-class
cache keyed by UClass* so a chipPile_erie_C resolves its OWN GetKey
(BP-overridden or inherited — engine resolves both at FindFunction time).

`IsSubclassOf` is a 3-line helper (SuperStruct walk vs target base, ≤16
hops) — local utility, not exposed externally.

### 4.3 Three call-site changes

#### (i) Init POST broadcast (`prop_lifecycle.cpp:169`)
Replace `if (!ue_wrap::prop::IsDescendantOfProp(self)) return;` with
`if (!ue_wrap::prop::IsKeyedInteractable(R::ClassOf(self))) return;`.
This is the gate that lets chipPile/clump/trashBitsPile broadcast a
**PropSpawn** at Init time — same packet shape, same wire path.

The Init POST observer is installed via the subclass-aware scan at
`prop_lifecycle.cpp:467`, which walks all UFunctions named "Init" whose
Outer is prop_C-descended. We need to extend that scan too — register
on Init UFunctions whose Outer is chipPile / clump / trashBitsPile
lineage as well. **Minimal change**: replace
`if (!ue_wrap::prop::IsClassDescendantOfProp(owningCls)) continue;` at
prop_lifecycle.cpp:467 with the new `IsKeyedInteractable(owningCls)`.

#### (ii) K2_DestroyActor PRE gate (`prop_lifecycle.cpp:290`)
Same swap: `IsDescendantOfProp` → `IsKeyedInteractable`. Now chipPile/
clump/trashBitsPile destroys also ride **PropDestroy** (same wire).

#### (iii) PropPose emit (`harness.cpp:468`)
Add a fallback to `holding_actor` when `grabbing_actor` is null:

```cpp
void* heldActor = *reinterpret_cast<void**>(
    reinterpret_cast<uint8_t*>(g_netLocal) + ue_wrap::reflected_offset::MainPlayer_grabbing_actor());
if (!heldActor || !R::IsLive(heldActor)) {
    // chipPile / clump / trashBitsPile pickup path uses holding_actor @0x0A20.
    heldActor = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(g_netLocal) + 0x0A20);   // mainPlayer.holding_actor
    if (heldActor && !R::IsLive(heldActor)) heldActor = nullptr;
}
```

This needs `MainPlayer_holding_actor()` added to `reflected_offset.cpp`
(reflection-driven offset rather than hard-coded 0x0A20) per the
[adaptation-strategy] doc — but the offset 0x0A20 is what the dump shows
today and the existing `grabbing_actor` accessor sets the precedent.

Plus: in `prop_wrap::GetKeyString(heldActor)`, the receiver-side lookup
`FindByKeyString` is gated on `IsDescendantOfProp` at prop.cpp:287. Swap
that to `IsKeyedInteractable` too (it's filter-only, won't false-positive
since the keys are namespace-distinct).

#### (iv) Snapshot enumeration (`prop_snapshot.cpp:64`)
Swap `IsDescendantOfProp` → `IsKeyedInteractable`. Connect-edge snapshot
now enumerates chipPile/clump/trashBitsPile instances too, riding the
existing PropSpawn pipeline.

#### (v) FindByKeyString / FindNearbySameClass (`prop.cpp:255 + 287`)
Both filter-only gates; swap to `IsKeyedInteractable`.

#### (vi) FindNearest (`prop.cpp:119`)
`FindNearest()` is used by the autonomous grab test only — and that test
specifically looks for Aprop_C derivatives (heavy/static filtering reads
`Aprop_propData_heavy @ +0x6C` which doesn't exist on chipPile/clump).
**Do NOT swap this one.** It must stay `IsDescendantOfProp`. Leaves the
contract clean: FindNearest is for the autonomous test's heavy-prop
scenario only.

### 4.4 The receiver-side PropSpawn path is already class-agnostic

`remote_prop.cpp::OnSpawn` (line 492+) uses `R::FindClass(classW.c_str())`
where `classW` is the wire class name (e.g. "actorChipPile_C"). The
spawn path does NOT call `IsDescendantOfProp`; it spawns whatever the
wire says. The setKey step (line 671) calls `Aprop_C.setKey` — BUT
chipPile/clump have their OWN `setKey` UFunctions (per dump). The
existing path resolves `g_propSetKeyFn` once from prop_C; for chipPile/
clump we need a per-class setKey resolution. Same pattern as GetKey
(§4.2).

Concretely: replace the singleton `g_propSetKeyFn` with
`ResolveSetKeyForClass(spawned->ClassOf())` — caches per-class. For
Aprop_C descendants it returns the prop_C setKey (the existing UFunction,
inherited by all subclasses). For chipPile/clump it returns their own
setKey. Both setKey UFunctions take `FName Key` as the lone param;
ParamFrame.SetRaw(L"Key", ...) works for both.

### 4.5 Throw path — open question

The PropRelease emit reads `prop::GetPhysicsVelocity(g_lastHeldProp)`
which reads the body's velocity via UFunctions on the prop's
`StaticMesh @ 0x0238`. Both chipPile and clump have a `StaticMesh @
0x0228 / 0x0230` field — DIFFERENT OFFSET. `prop_wrap::GetStaticMesh`
reads `*(prop + 0x0238)` — for chipPile/clump that's `Collision` or
`chipType`, not the mesh.

Per-class mesh offset needed (mirror the GetKey table):
- Aprop_C → 0x0238
- AactorChipPile_C → 0x0228 (StaticMesh field; the BP-default-pickable mesh)
- Aprop_garbageClump_C → 0x0230
- AtrashBitsPile_C → 0x0250

Add a `prop_wrap::GetStaticMesh(prop)` per-class dispatch. Same form as
GetKey.

Then `prop::GetPhysicsVelocity` reads the velocity off the right
component, and the existing PropRelease emit/receive (which uses
`SetPhysicsLinearVelocity` on the receiver's mesh) works unchanged.

**Caveat from morph RE §4.3 / §4.4**: clump may NOT be in
PhysX-simulating state while held (per the candidate-C "attach-to-actor"
hypothesis the clump might be in kinematic mode while attached). In that
case GetPhysicsVelocity returns ~zero and the launched clump on the
receiver has no momentum. This needs a runtime probe (§6 OPEN-A).
Workaround if confirmed: read velocity from mainPlayer's
`throwHoldingProp` param frame via a PRE observer (Phase 5G STAGE 2's
H2 hook from the morph RE doc §6.3).

---

## 5. RULE-2 retirement of the STAGE 2 `non_prop_entity_sync` stack

If §4 ships and chipPile/clump/trashBitsPile broadcast via the existing
Aprop_C pipeline, the parallel STAGE 2 infrastructure becomes baggage.
**Everything to delete:**

### 5.1 Whole files to delete

- `src/votv-coop/include/coop/non_prop_entity_sync.h` (89 LOC)
- `src/votv-coop/src/coop/non_prop_entity_sync.cpp` (770 LOC)

### 5.2 `CMakeLists.txt` (`src/votv-coop/CMakeLists.txt:78`)

Delete the line:
```
    src/coop/non_prop_entity_sync.cpp
```

### 5.3 `protocol.h` (`src/votv-coop/include/coop/net/protocol.h`)

- Delete the comment block around ReliableKind 16/17 (lines ~219-233).
- Delete `ReliableKind::NonPropEntityState = 16` and
  `ReliableKind::NonPropEntityDestroy = 17` entries.
- Delete `enum class NonPropEntityClass` (line 508+).
- Delete `struct NonPropEntityStatePayload` (line 519+) + its 2
  static_asserts.
- Delete `struct NonPropEntityDestroyPayload` (line 535+) + its 2
  static_asserts.

### 5.4 `session.h` / `session.cpp`

- `include/coop/net/session.h:148-153`: delete `SendNonPropEntityState` +
  `SendNonPropEntityDestroy` declarations + their preceding comment block.
- `src/coop/net/session.cpp:158-163`: delete the two function bodies.

### 5.5 `event_feed.cpp` (`src/votv-coop/src/coop/event_feed.cpp`)

- Line 5: delete `#include "coop/non_prop_entity_sync.h"`.
- Lines 384-415 (approx): delete the two `case` blocks for
  `NonPropEntityState` + `NonPropEntityDestroy`.

### 5.6 `harness.cpp` (`src/votv-coop/src/harness/harness.cpp`)

- Line 16: delete `#include "coop/non_prop_entity_sync.h"`.
- Lines 297-298: delete the `SetSession` / `Install` calls.
- Line 373: delete `coop::non_prop_entity_sync::OnDisconnect();`.
- Lines 387-394: delete the `// Phase 5G Inc 2 STAGE 2 ...` block +
  `ReplaySnapshotForJoinedClient()` call.
- Lines 439-443: delete the `// Phase 5G Inc 2 STAGE 2 ...` block +
  `DrainRetryQueues()` call.

### 5.7 `prop_lifecycle.cpp` (`src/votv-coop/src/coop/prop_lifecycle.cpp`)

- Line 9: delete `#include "coop/non_prop_entity_sync.h"`.
- Lines 271-289 (the entire `// Phase 5G Inc 2 ...` block inside
  `GrabObserver_Actor_K2DestroyActor_PRE`): delete the
  `IdentityForDestroyingActor` branch. With chipPile/clump/trashBitsPile
  flowing through `IsKeyedInteractable` → standard PropDestroy path,
  this routing is redundant.

### 5.8 What about `garbage_sync.cpp` (Inc 1 + Inc 3)?

`garbage_sync.cpp` has two independent install paths:
- **Inc 1**: PRE-cancel `Aprop_openContainer_C::ReceiveTick + checkPickup`
  on the client. Root cause was `itemsInside @ +0x378` walking stale
  AActor* of per-peer-divergent garbage containers.
- **Inc 3**: 4 spawner suppressors (undergroundGarbageSpawner.Timer,
  event_trashPiles.BndEvt, arirTrasher.trash,
  baseCleaner_trashBits.BeginPlay). Mirror of the mushroom-spawner
  client-suppression pattern.

**Inc 3 (spawner suppressors): KEEP.** Per RULE-1: these prevent the
client from running its own divergent garbage spawn rolls, which is the
root cause of `itemsInside` holding divergent ptr lists. The chipPile
sync layer would only resolve symptoms (per-actor lifecycle) — without
the suppressors, both peers' spawners still roll independently and
produce a 2x-broadcast / fuzzy-key-mismatch storm exactly as the mushroom
case did. The mushroom precedent is already shipped and proven right.

**Inc 1 (openContainer per-tick cancel): RETIRE — pending verification.**
Once Inc 3 suppressors hold, the client never independently spawns
garbage entities, so `itemsInside` only contains host-broadcast actors
that DO exist on the client (because PropSpawn delivered them). The
stale-AActor* AV root cause goes away. Inc 1 becomes a CRUTCH (a
broad-suppression filter; cancels BP every tick) — RULE-1 violation
under the new architecture.

Retirement procedure per RULE-1's "transitional crutch with retirement plan":
1. Ship §4 (chipPile-into-Aprop pipeline) + keep Inc 1 enabled.
2. Hands-on test: host has a garbage container with items, client picks
   up garbage. Confirm no AV with Inc 1 still ON.
3. Disable Inc 1 (delete the openContainer PRE-cancel, leave Inc 3
   spawner suppressors).
4. Repeat the test. If still no AV → delete Inc 1 from
   `garbage_sync.cpp::Install()` permanently (RULE-2). Inc 1's
   `g_garbageContainerCls`, `OnOpenContainerReceiveTickPre`,
   `OnOpenContainerCheckPickupPre`, and the openContainer/garbageContainer
   class resolution at lines 216-235 — DELETE.
5. If the AV resurfaces, the root cause is something else (probably a
   stale entry in `itemsInside` left over from a save predating the
   sync layer) and Inc 1 isn't the real fix anyway — needs deeper RE.

Until step 4 is done, Inc 1 stays. Document as a transitional crutch
with the explicit gating criterion: "Inc 1 retires when chipPile-into-
Aprop pipeline ships AND a hands-on garbage-container test passes
without it."

### 5.9 Reference removals (sanity)

`grep -r non_prop_entity_sync src/votv-coop/` should return zero hits
post-retirement except in the deletion commit's own changelog.

---

## 6. Open questions / probes needed before shipping

> **[corr 2026-09-04: THIS SECTION IS HISTORICAL -- the lane SHIPPED.** `docs/piles/11` records
> "nativization AS-BUILT + VERIFIED (hands-on)" on 2026-06-30, so these are not probes still owed
> before shipping. What shipped is the CRUTCH form catalogued as C2 in `docs/CRUTCHES.md`, whose
> retirement is queued as `PERF_ARC` Q1 -- read that, not this section, for the live state.]**

### 6.1 OPEN-A: held-pose mechanism for chipPile/clump

Per morph RE §4: three candidates for what drives the clump's world
transform while held —
- (A) clump.ReceiveTick reads holdPlayer and SetActorLocations itself.
- (B) mainPlayer drives via holding_actor in its own Tick.
- (C) K2_AttachToActor at toClump() time; engine drives the attach.

(C) is the strongest hypothesis. Resolution matters because:
- If (A) or (B): clump is simulating; GetPhysicsVelocity reads return
  meaningful values during hold; PropPose-via-holding_actor (§4.3 (iii))
  works directly.
- If (C): clump is kinematic/attached; SetActorLocation we'd issue from
  PropPose receive would **fight** the engine's attach. We'd need to read
  whether the wire-spawned clump is attached on the receiver and detach
  it on PropPose arrival, OR PropPose has to carry attach state too.

**Probe needed**: hook clump's Init POST on the host, immediately
poll `actor.GetAttachParentActor()` via reflection. If non-null →
candidate (C). Cheap UE4SS Lua probe before changing anything.

### 6.2 OPEN-B: trashBitsPile pickup semantics

The morph RE doc covers chipPile + clump; trashBitsPile was only
catalogued as "shares the prop-like UFunctions". Need to verify whether
trashBitsPile.playerTryToHold:
- spawns a different held actor (like chipPile → clump), OR
- collects into inventory + destroys self (most likely — it's a
  "trashBits" sweepable, behaves like sawdust), OR
- attaches itself to the player.

If "collects into inventory + destroys self": the destroy IS the only
event, no held-pose stream needed. The existing K2_DestroyActor PRE
broadcast covers it. PropPose emit is irrelevant for trashBitsPile.

**Probe needed**: BP-runtime trace of trashBitsPile.playerTryToHold
once on a live test. 10-minute probe.

### 6.3 OPEN-C: dirtball

`Aprop_dirtball_C` is mentioned in morph RE §6.4 as missing from STAGE 2
class table. With the IsKeyedInteractable approach (§4.1) it's covered
automatically — it's a subclass of `Aprop_garbageClump_C` so the
SuperStruct walk catches it. **No extra work**; this is a nice side
effect of doing the right thing in §4.

### 6.4 OPEN-D: clump.GetKey behaviour when first spawned

Init POST reads GetKey via UFunction; if the BP author's GetKey returns
NAME_None on a fresh-spawn clump (because the BP-VM Key field hasn't
been written yet), our key-empty-or-"None" guard at
`prop_lifecycle.cpp:221` would drop the broadcast. **Probe**: on a live
host, hook chipPile/clump Init POST and log the GetKey()-returned name.
If "None" consistently → we'd need to first call setKey(NewGuid) on the
host before broadcast (mirror what Aprop_C's Init does internally).
This is a Stream A item: investigate before shipping.

### 6.5 OPEN-E: chipPile/clump setKey vs Aprop_C setKey

Wire receiver spawns the actor then calls setKey to align Keys across
peers. The current `g_propSetKeyFn` is resolved from
`R::FindClass(P::name::PropClass)` (prop_C). For chipPile/clump, we'd
need per-class setKey resolution. Confirmed in §4.4 above; no probe
needed, just the implementation.

### 6.6 OPEN-F: the BP-VM-only Key offset

The CXX dump format reports only **native** UProperty fields. BP-VM
variables (declared as `Variable` in the BP editor with no "Native" tag)
are stored in a parallel UScriptStruct addressed via the
`UberGraphFrame` pointer. For chipPile/clump, the Key is almost certainly
a BP-VM variable. **This means we can NEVER reach the Key via a fixed
native offset; the BP UFunction `GetKey`/`setKey` is the only stable
read/write path.** Confirmed by the absence of `FName Key` in the
struct dump.

This is RULE-1 acceptable: BP UFunctions ARE the canonical accessor —
they're what every save/load and interaction code path uses internally.
We're not bypassing anything.

### 6.7 OPEN-G: throw velocity capture (per §4.5)

If clump is kinematic-while-attached (OPEN-A candidate C), the
SetPhysicsLinearVelocity-then-release path may differ. Need a probe at
release time on the host: log clump.StaticMesh velocity AT the moment
`playerHandRelease_LMB` fires + at the next ReceiveTick. Determines
whether velocity-on-release is captured by the existing post-release
GetPhysicsVelocity read or needs the param-frame capture from
`throwHoldingProp` PRE.

---

## 7. Summary of code changes

### 7.1 Add (one new helper + per-class accessors)

- `IsKeyedInteractable(void* cls)` in `ue_wrap/prop.h` + `prop.cpp` (~40 LOC)
- Per-class GetKey dispatch in `prop_wrap::GetKey` (~30 LOC; cache + helper)
- Per-class GetStaticMesh dispatch in `prop_wrap::GetStaticMesh` (~20 LOC)
- Per-class setKey dispatch in `remote_prop.cpp::OnSpawn` (~25 LOC)
- `reflected_offset::MainPlayer_holding_actor()` (~5 LOC for the offset
  accessor; or hard-code 0x0A20 with a `// TODO reflection` comment).

### 7.2 Swap (replace `IsDescendantOfProp` / `IsClassDescendantOfProp` at
filter-only gates; keep where strictly Aprop_C-shape semantics matter)

- `prop_lifecycle.cpp:169` — swap (broadcast gate)
- `prop_lifecycle.cpp:290` — swap (broadcast gate)
- `prop_lifecycle.cpp:346` — swap (takeObj POST extracted-actor gate)
- `prop_lifecycle.cpp:467` — swap (Init scan owningCls gate)
- `prop_snapshot.cpp:64` — swap (snapshot enumeration)
- `prop.cpp:255` — swap (FindNearbySameClass)
- `prop.cpp:287` — swap (FindByKeyString)
- `prop.cpp:119` — KEEP `IsDescendantOfProp` (FindNearest: autonomous
  test heavy-prop scenario, reads Aprop-specific propData.heavy field)

### 7.3 Extend (PropPose emit)

- `harness.cpp:468-469` — extend held-actor read to fall back from
  `grabbing_actor` to `holding_actor` (one if-block).

### 7.4 Delete (STAGE 2 retirement, RULE-2)

- `src/votv-coop/include/coop/non_prop_entity_sync.h` (whole file)
- `src/votv-coop/src/coop/non_prop_entity_sync.cpp` (whole file)
- `src/votv-coop/CMakeLists.txt:78` (the source list entry)
- `protocol.h`: ReliableKind 16/17 entries + NonPropEntityClass enum +
  NonPropEntityStatePayload + NonPropEntityDestroyPayload + asserts.
- `session.h:148-153` + `session.cpp:158-163` — `SendNonPropEntity*`
- `event_feed.cpp:5,384-415` — include + cases
- `harness.cpp:16,297-298,373,387-394,439-443` — includes + 6 call sites
- `prop_lifecycle.cpp:9,271-289` — include + IdentityForDestroyingActor
  branch in K2_DestroyActor PRE

### 7.5 Conditional delete (after retirement gate, §5.8)

- `garbage_sync.cpp` Inc 1 (openContainer ReceiveTick + checkPickup
  PRE-cancels) — RETIRE after hands-on test confirms Inc 3 alone
  prevents the AV. Keep Inc 3 spawner suppressors.

---

## 8. Why this is RULE-1 compliant

- We do NOT carve out chipPile/clump as a special case. They ride the
  SAME wire packets (PropSpawn, PropDestroy, PropPose, PropRelease) as
  every other interactable. The only per-class difference is the Key /
  StaticMesh accessor — addressed properly via per-class UFunction /
  field dispatch tables, NOT a "skip-if".
- The Key reader for chipPile/clump uses the BP-author's canonical
  `GetKey()` UFunction — same dispatch path the save system uses.
- The PropPose emit fallback (`holding_actor` after `grabbing_actor`)
  is targeted: where the BP graph clearly uses a different held-actor
  slot, we read THAT slot. Not a broad workaround.
- STAGE 2's non_prop_entity_sync is retired per RULE-2; no dual paths
  remain. STAGE 2 was the PRIOR ATTEMPT at this problem — replacing it
  fully is RULE-1 + RULE-2 in concert.

## 9. Why this is RULE-2 compliant

- The non_prop_entity_sync.cpp + .h files are DELETED, not deprecated.
- Wire packets ReliableKind 16/17, payload structs, enum NonPropEntityClass,
  and their session sender/receiver helpers are DELETED.
- harness.cpp, event_feed.cpp, prop_lifecycle.cpp lose their includes +
  call sites entirely.
- No feature flag, no parallel path. The replacement path (Aprop_C
  pipeline + IsKeyedInteractable) is the ONE path post-ship.
- Inc 1 of garbage_sync becomes a transitional crutch with a CLEAR
  retirement gate (hands-on test, §5.8 step 3-4) and goes too as soon as
  that test passes.

---

## 10. Effort estimate

- §4 implementation (helpers + swap + extend): ~200 LOC across 6 files.
- §5 retirement: ~900 LOC deleted across 7 files.
- Net change: ~-700 LOC. Simpler.
- Risk: low — the receive path is class-agnostic today; the send path
  changes are gate-relaxations + a fallback read.
- Probes needed before shipping (§6.1, §6.2, §6.4, §6.7): ~30 min total
  via UE4SS Lua + a host hands-on.

End of doc.

---

## Probe answers (static RE, 2026-05-27 follow-up)

Sources used in this pass:
- CXX dump: `actorChipPile.hpp`, `prop_garbageClump.hpp`, `trashBitsPile.hpp`,
  `actor_save.hpp`, `prop.hpp`, `mainPlayer.hpp`, `prop_dirtball.hpp`,
  `prop_garbageClump_erie.hpp`.
- IDA Pro MCP on `VotV-Win64-Shipping.exe` (idb path: `Game_0.9.0n/WindowsNoEditor/
  VotV/Binaries/Win64/VotV-Win64-Shipping.exe.i64`):
  - `find_regex` on `prop_garbageClump`, `garbageClump`, `actorChipPile`,
    `ChipPile`, `trashBitsPile`, `playerTryToHold`, `mainPlayer`,
    `grabbing_actor`, `holdPlayer` — **zero matches each**.
  - `find_regex` on engine-native names: `K2_AttachToActor` (1 hit @
    0x144146f40), `K2_SetActorLocation` (1 hit @ 0x144147040),
    `GrabComponentAtLocation` (1 hit @ 0x1441f9e40),
    `SetPhysicsLinearVelocity` (1 hit @ 0x1442061e0),
    `GetAttachParentActor` (1 hit @ 0x144146ac0), `setKey` (0 hits as a
    VOTV BP UFunction — only `EControlRigSetKey` engine enum).
  - `xrefs_to 0x144146f40` (K2_AttachToActor string) — 3 data xrefs (FName
    pool table entries), zero code xrefs. Confirms the string is an
    engine FName pool entry, not a native function pointer.

### Structural finding: BP bytecode is not in the EXE

All four open questions require reading the bodies of BP UFunctions
(`AactorChipPile_C::toClump`, `Aprop_garbageClump_C::ReceiveTick`,
`playerHandRelease_LMB`, `setKey`/`GetKey`, etc.). UE4.27 compiles BP to
bytecode stored in cooked `.uasset` files; the bytecode is interpreted at
runtime by `UObject::ProcessEvent` → BP VM (`ProcessInternal`). The
shipping EXE contains the engine + native code only — **none of the
BP UFunction bodies for the three VOTV classes are visible to IDA**.

Static-analysis evidence: zero string hits for any VOTV BP class name or
BP UFunction name in the entire EXE strings cache (77,821 strings
indexed). Only the engine-defined names appear (those engine UFunctions
which BP calls into).

Repo also has no extracted `.uasset` for these classes (`glob **/prop_
garbageClump*.uasset` → 0 results, same for actorChipPile and
trashBitsPile). The `.pak` is not unpacked.

**This means each of the four probes that needs BP-body inspection is
genuinely unanswerable statically from the current artifact set.** The
strongest path forward is small, gated runtime reflection reads — what I
write up per probe below.

What CAN be answered statically is:
- Field offsets, types, presence/absence (CXX dump).
- UFunction NAMES per class (CXX dump).
- Field-cluster shapes that constrain the candidate space.
- Sibling-class pattern matches (Aprop_C, Aactor_save_C, prop_dirtball).

---

### OPEN-A — held-pose mechanism for clump

**Constraints from static reads:**
1. `Aprop_garbageClump_C::ReceiveTick(float DeltaSeconds)` IS declared
   (`prop_garbageClump.hpp:105`). Per-tick BP body exists.
2. `holdPlayer @ 0x0240` (AmainPlayer_C*) — per-instance owner pointer.
   Has no meaning unless something READS it. Only ReceiveTick body or
   mainPlayer's hold-driver-tick can be the reader.
3. `canConvert @ 0x0248`, `delayOnHit @ 0x0249`, `initLaunch @ 0x0250`,
   `Max @ 0x024C`, `Slope @ 0x02F0`, `Hit @ 0x0268` (FHitResult) — a
   state-machine field cluster CONSISTENT with per-tick hold→launch→hit
   flow.
4. NO `K2_AttachToActor` declared on clump (would appear in its
   UFunction list if BP-overridden). The engine's `K2_AttachToActor` is
   callable from BP without being overridden, however — its absence
   from the dump doesn't rule out a one-shot call from inside toClump.
5. mainPlayer has `holding_actor @ 0x0A20` + `holdObjectChanged_pre/post`
   delegates @ 0x0DA0/0x0DB0 — clearly a generic "I'm carrying something
   manually" pipeline (separate from `grabbing_actor @ 0x07D0` PHC).
6. mainPlayer has `Hold Object(bool useHold, AActor* Manual, bool& collected)`
   (mainPlayer.hpp:464) — the BP-graph hold dispatcher. Confirmed
   present statically; body in BP bytecode.

**Static verdict — between candidates:**
- **Rule OUT (A) `UPhysicsHandleComponent::GrabComponentAtLocation`** —
  confirmed in prior morph RE (§1.1) and corroborated by the absence of
  any PHC fan-out for clump. mainPlayer's `grabbing_actor` is separate
  from `holding_actor`.
- **Cannot statically rule between (B) per-tick `K2_SetActorLocation` and
  (C) `K2_AttachToActor`.** Both are engine-native callable from BP.
  Field cluster on clump (ReceiveTick + Hit + Slope + delayOnHit) leans
  toward (B) — there's NO field on clump that looks like
  "AttachParentRecord" / "RelativeLocationCache", which an attached
  actor wouldn't strictly need but heavy-physics-detach paths often
  cache. The presence of a per-tick BP body is evidence for (B): if the
  engine handled the attach natively (C), there'd be no need for a
  ReceiveTick body on the clump.
- **Mild lean toward candidate (B)** based on field-cluster shape. Not
  conclusive.

**RUNTIME PROBE — minimal, ≤5 LOC, append to `non_prop_entity_sync` or new
`held_entity_sync.cpp`:** at clump Init POST, log
`GetAttachParentActor()` once. If non-null → (C); if null → (B):

```cpp
// Inside clump Init POST observer (host-side):
void* parentActor = nullptr;
R::CallFunction(self, g_getAttachParentActorFn, &parentActor);
LOG_INFO("[probe-A] clump=%p Init POST GetAttachParentActor=%p",
         self, parentActor);
```

If parentActor != nullptr → candidate (C); host attached clump to itself
or its mesh in toClump. Receiver would need an attach instruction.

If parentActor == nullptr at Init POST → candidate (B); per-tick drive.
For the integration plan §4.3 (iii) (PropPose-via-holding_actor) this is
the **better outcome**: the host's per-tick world-transform updates will
broadcast naturally via PropPose. No new attach packet needed.

**Implications for integration plan:**
- If probe confirms (B): integration plan §4.3 (iii) as-written is
  correct; PropPose-via-holding_actor handles the held stream.
- If probe confirms (C): integration plan needs an `EntityAttach` packet
  (§6.2 of the morph RE doc) layered on top. The morph RE doc already
  has this design — wire it instead of dropping it.

---

### OPEN-B — trashBitsPile pickup semantics

**Static evidence — overwhelming for "dispenser into inventory, not held actor":**

Read trashBitsPile.hpp fully. Field cluster (`trashBitsPile.hpp:6-26`):
- `Shape: int32` @ 0x0258 — visual shape selector (decal-mesh variant).
- `typeA, typeB: enum_trashType` @ 0x025C, 0x025D — two dispense types.
- `randomTypeA, randomTypeB: bool` @ 0x025E, 0x025F.
- `amountA, amountB: int32` @ 0x0260, 0x0264 — **counters of remaining bits**.
- `snapToSurface, AlignToNormal, randomYRoll, doDebug: bool` — decal placement.
- 8x `tt_plastic / tt_paper / tt_metal / tt_rubber / tt_elec / tt_glass /
  tt_wood / tt_organic: bool` @ 0x026C-0x0273 — type-filter set.

UFunction set (`trashBitsPile.hpp:28-77`):
- `getRandomTrashType() -> enum_trashType` (line 44) — pulls a type from
  the filter set.
- `getTrashProp(out bool last, out FName prop)` (line 50) — **returns a
  PROP NAME (FName), not an actor reference**. The `last` out-bool says
  "this dispense empties the pile".
- `trashToProp(enum_trashType, out FName prop)` (line 51) — type-to-prop-
  name mapping. Same FName-only return.
- `D(FName InputPin)` (line 48) — Dispense entry point.
- `DebugName()` (line 49).
- `playerTryToHold`, `playerTryToGrab`, `playerTryToCollect` (lines 31,
  30, 33) — pickable-interface UFunctions, present.
- `playerGrabbed(Player, HitResult)`, `playerGrabbed_pre`,
  `beginHoldingObject` (lines 75, 68, 69) — pickup-flow UFunctions.

**Critical omissions** that rule out the held-actor path:
- **NO `holdPlayer` field** (compare clump @ 0x0240 — present).
- **NO `ReceiveTick`** declared (compare clump line 105 — present).
- **NO `Init`** declared (compare clump line 51 + chipPile line 65 — present).
- **NO `playerHandRelease_LMB/RMB`** declared (compare clump lines 70-71,
  chipPile lines 78-79 — present).
- **NO `playerHandUse_LMB/RMB`** declared (compare clump/chipPile —
  present).
- **NO `unequpped`** declared (compare chipPile line 123 — present).
- **NO `toClump`** equivalent (no morph spawn path).
- **NO `physDestroyed` / `physPreDestroyed`** declared.

**Verdict: trashBitsPile is an INVENTORY DISPENSER, not a held actor.**

The likely playerTryToHold body (reconstructed from declarations only,
NOT decompiled — BP body is unreachable statically):

```
playerTryToHold(Player, out collected):
    if amountA > 0 or amountB > 0:
        prop_name = getTrashProp(out last) -> trashToProp(...) -> FName
        Player.inventory.takeObj(prop_name)   // or similar give-item path
        decrement counter (amountA or amountB)
        if last: K2_DestroyActor(self)
        collected = true
    else:
        collected = false
```

This is a save-Key-stable actor — it inherits from `Aactor_save_C` so
Key @ 0x0230 is a real native field. State that needs sync across peers:
- The `amountA` / `amountB` counters (per-pickup decrement).
- The destroy edge when last==true.

**Implications for integration plan:**

1. **trashBitsPile does NOT need a PropPose held-pose stream.** Its
   coop-relevant events are:
   - Init POST (Phase 5G STAGE 2 already covers — also covered by §4 of
     this doc once gate is swapped).
   - K2_DestroyActor PRE (when emptied — §4 swap covers).
   - **NEW**: `amountA`/`amountB` decrement edge — a per-pickup counter
     wire packet. **Out of scope for the chipPile-into-Aprop wiring**;
     this is a separate trashBits-counter sync feature.

2. **Holding-actor branch in §4.3 (iii) is NOT triggered for
   trashBitsPile.** It would be triggered for chipPile (which sets
   mainPlayer.holding_actor to the freshly-spawned clump via the
   toClump morph) and clump (which IS the held actor). trashBitsPile
   never lands in mainPlayer.holding_actor.

3. **Recommendation**: keep trashBitsPile under §4 (gate swap to
   IsKeyedInteractable) for its Init / Destroy broadcasts, but EXPLICITLY
   note in §4 that trashBitsPile is destroy-edge-only. **Add a
   follow-up FUTURE entry**: "trashBitsPile dispense-counter sync" [corr 2026-09-04: SHIPPED
   v57 2026-06-10 -- `event_dispatch_state.cpp:305` "trashBitsPile collect counters -- SYMMETRIC"] — a
   per-pickup decrement broadcast, mirror of the storage-container
   takeObj pattern. Not blocking ship of chipPile-into-Aprop wiring.

4. **trashBitsPile's Key IS @ +0x0230 (inherited from Aactor_save_C).**
   The integration plan §4.2 path already correctly handles this. No
   change needed.

---

### OPEN-D — fresh clump's Key behaviour

**Static reads:**
- `Aprop_garbageClump_C` is `: public AActor` directly. No native Key
  field anywhere in struct dump (size 0x2F4 fully accounted: physicsImpact,
  StaticMesh, chipType, holdPlayer, canConvert, delayOnHit, Max,
  initLaunch, skipSave1, LifeSpan, pile, Hit, Slope — confirmed 14
  fields, no Key).
- `GetKey(FName& Key)` UFunction declared (`prop_garbageClump.hpp:44`).
  `setKey(FName Key)` UFunction declared (`prop_garbageClump.hpp:55`).
  BP bodies not in EXE.
- `skipSave1 @ 0x0254` — strongly implies clump is NOT save-persisted
  (the chipPile is; the clump is transient and re-spawned from its
  parent chipPile on load). `Aprop_C`'s `ResetKey @ 0x0362` has a
  similar role.

**Reconstruction constraint:**

Since the clump is transient + spawned mid-game via `toClump()` (not
loaded from save), there is NO save-Key for it to derive a Key from.
The most likely BP bodies (compare Aprop_C precedent, see prop.hpp:181):

Hypothesis A (clump.GetKey returns NAME_None):
- BP-VM Key variable starts uninitialised → reads as `NAME_None`.
- setKey writes to the BP-VM Key variable.
- Save flow on a fresh clump never runs (skipSave1) so the BP-VM Key
  never gets backed by a NewGuid.
- **Aprop_C's Init pattern**: Aprop_C.Init internally generates a NewGuid
  and writes it via setKey if Key is None. **Clump's Init body is
  unreachable statically** — cannot confirm whether clump.Init does the
  same NewGuid auto-mint or skips it.

Hypothesis B (clump.GetKey reads from chipPile-source):
- toClump() runs setKey on the new clump using the source chipPile's
  Key, so identity is propagated. **Cannot confirm without BP body.**

**Mild static lean toward Hypothesis A** based on the skipSave1 flag —
transient actors typically don't bother with Key minting. The BP author's
intent matters here and can't be read from EXE.

**RUNTIME PROBE — ≤5 LOC, append to clump Init POST observer:**

```cpp
// Inside clump Init POST observer (host-side), one-shot per first clump spawn:
R::FName keyOut{0, 0};
R::CallFunction(self, g_clumpGetKeyFn, &keyOut);
const wchar_t* kw = R::FNameToString(keyOut);
LOG_INFO("[probe-D] clump=%p Init POST GetKey=%ls (entry=%u num=%u)",
         self, kw ? kw : L"<null>", keyOut.entry, keyOut.num);
```

If GetKey returns `None` (entry=0, num=0): host MUST call setKey(NewGuid)
on the clump BEFORE broadcasting Init POST. Mirror the Aprop_C path —
prop_lifecycle.cpp:221 already has this pattern.

If GetKey returns a meaningful name (likely matching source chipPile's
Key): identity is auto-propagated via toClump's BP body. Host
broadcasts as-is.

**Implications for integration plan §4.2:**
- The `GetKeyViaUFunction` path is correct shape-wise.
- The integration plan §4.4 already proposes per-class setKey resolution
  for receiver-side OnSpawn. **Add a corresponding host-side
  pre-broadcast setKey call** if probe-D returns NAME_None: the host's
  Init POST observer must mint a NewGuid and setKey(NewGuid) BEFORE
  emitting PropSpawn. Otherwise the receiver's mirror clump will have a
  fresh NewGuid that doesn't match the host's Key, breaking subsequent
  PropPose / PropRelease keyed routing.

---

### OPEN-G — throw mechanism and velocity capture

**Static evidence — multi-source convergence on candidate "(a) detach + impulse on clump itself, no destroy":**

1. clump declares `playerHandRelease_LMB(Player)` and
   `playerHandRelease_RMB(Player)` (`prop_garbageClump.hpp:70-71`).
   BP bodies unreachable.
2. mainPlayer declares `throwHoldingProp()` (line 443) and
   `throwShit(UPrimitiveComponent* InputPin, FVector NewVel)` (line 470).
   The `throwShit` signature is **load-bearing**: it takes a
   UPrimitiveComponent + an explicit NewVel FVector. This is the engine
   "apply velocity to this component" entry point.
3. clump's StaticMesh @ 0x0230 is a `UStaticMeshComponent*` — the body
   that physics drives.
4. clump's `Hit @ 0x0268` (FHitResult, 0x88) + `Slope @ 0x02F0` + the
   `BndEvt__prop_garbageClump_StaticMesh_K2Node_ComponentBoundEvent_0_
   ComponentHitSignature` delegate (line 103) — the clump LISTENS to its
   own StaticMesh's ComponentHit event. This means the clump IS physics-
   simulated post-throw (otherwise ComponentHit wouldn't fire).
5. **Engine confirmation**: `SetPhysicsLinearVelocity` string present at
   0x1442061e0 in the EXE. This is the canonical "give the body a
   velocity" entry. Almost certainly what `throwShit` calls into.
6. The Aprop_C existing release path (per integration plan §4.5):
   `grab_observer::GrabObserver_PrimComp_SetLinearVelocity_PRE` observes
   `SetPhysicsLinearVelocity` engine-wide and fires for any prim
   component receiving a velocity. This PRE observer is currently
   diagnostic-only (NOT linked to PropRelease emit per integration plan
   §3 footnote).

**Static verdict — answer to OPEN-G:**

- Release mechanism is candidate **(a) detach (if attached) + apply velocity
  on clump's StaticMesh; clump survives the launch, physics carries it
  through to the BndEvt hit handler**. The morph-back to chipPile happens
  on LANDING-HIT (the BndEvt branch), not on RELEASE — confirmed by the
  morph RE doc's prior reasoning AND consistent with the clump's
  ComponentHit listener.
- The launch velocity is supplied via `mainPlayer.throwShit(NewVel)`,
  which the existing `GrabObserver_PrimComp_SetLinearVelocity_PRE`
  observer SEES (PrimComp == clump.StaticMesh, NewVel == throw velocity).
- **No new observer is needed for velocity capture** — the existing PRE
  observer already fires at the correct moment with both required pieces.
  What's needed is: bridge the observer to a PropRelease emit when the
  PrimComp's outer is a keyed-interactable (chipPile/clump/Aprop_C).

**Implications for integration plan §4.5:**

The integration plan's §4.5 worry — "clump may not be PhysX-simulating
while held → GetPhysicsVelocity returns zero at release moment" —
**partially resolved statically**:

- IF OPEN-A resolves to candidate (B) per-tick: clump is detached/simulating
  the whole time. GetPhysicsVelocity reads at the host release-edge are
  meaningless (clump was driven kinematically by SetActorLocation; no
  velocity accumulated). The throwShit NewVel param IS the velocity, NOT
  the body's current velocity. So integration plan §4.5's
  "GetPhysicsVelocity → PropRelease body velocity" path is WRONG for
  clump — the host has to capture velocity from the SetLinearVelocity
  PRE observer's NewVel param, not from a post-release GetPhysicsVelocity.

- IF OPEN-A resolves to candidate (C) attached: same conclusion — clump
  is kinematic-attached during hold, throwShit applies NewVel to the
  freshly-detached body.

**Recommended path (regardless of OPEN-A outcome):**

Wire the `GrabObserver_PrimComp_SetLinearVelocity_PRE` observer to emit
PropRelease when:
1. `self->GetOwner()` is a keyed-interactable (chipPile / clump / Aprop_C
   descendant — `IsKeyedInteractable(R::ClassOf(self->GetOwner()))`).
2. The owner's Key (via `prop_wrap::GetKey`) is non-None.

Capture the NewVel argument from the param frame (PrimComp.SetLinearVelocity
takes `FVector NewVelocity, NAME BoneName, bool bAddToCurrent` — first
arg). Emit PropRelease{key, vel=NewVel}.

This is the **clean answer** — and it ALSO covers the existing Aprop_C
throw path (currently going through some other emit path; consolidates).

**Update integration plan §4.5:**
- Remove `prop::GetStaticMesh per-class dispatch` from §4.5.
- Remove `prop::GetPhysicsVelocity` reads at PropRelease emit time.
- Replace with: PropRelease emit driven from
  `GrabObserver_PrimComp_SetLinearVelocity_PRE`, gated on
  `IsKeyedInteractable(owner)`, velocity from the PRE observer's param
  frame. **Simpler, single-path, RULE-1 compliant.**
- The per-class GetStaticMesh dispatch IS still needed for any path that
  reads the body component directly (e.g. propagating sleep state) — but
  not for release-velocity capture.

### Summary of probe answers

| Probe | Answer | Path |
|---|---|---|
| OPEN-A | Cannot fully resolve statically. Mild lean toward (B) per-tick. PROBE: log `GetAttachParentActor()` at clump Init POST. | Runtime, 1 reflection read |
| OPEN-B | trashBitsPile is INVENTORY DISPENSER, not held actor. Static-confident. Destroy-edge only for sync. | Static-confirmed |
| OPEN-D | Cannot fully resolve statically. Mild lean toward NAME_None on fresh clump. PROBE: log GetKey at clump Init POST. | Runtime, 1 reflection read |
| OPEN-G | Throw = detach + SetLinearVelocity on clump's StaticMesh via mainPlayer.throwShit(NewVel). Capture NewVel from existing PrimComp_SetLinearVelocity PRE observer. Velocity from param frame, not from post-release GetPhysicsVelocity. | Static-confirmed (with morph-RE corroboration) |

### Recommendation changes to integration plan

1. **§4.3 (iii) PropPose emit fallback to `holding_actor`**: GATE on
   OPEN-A. If probe-A confirms (B) per-tick — ship as written. If probe-A
   confirms (C) attached — drop the fallback and replace with the
   morph-RE's `HeldEntityAttach` packet (§6.2 in
   `votv-chippile-clump-morph-RE-2026-05-27.md`).

2. **§4.5 throw path / per-class GetStaticMesh dispatch**: REPLACE.
   Drive PropRelease emit from
   `GrabObserver_PrimComp_SetLinearVelocity_PRE` (already exists,
   currently diagnostic-only). Gate on `IsKeyedInteractable(owner)`.
   Velocity from observer's param frame.

3. **§4.4 receiver-side setKey resolution**: KEEP. Add corresponding
   host-side pre-broadcast setKey if probe-D returns NAME_None.

4. **§4 for trashBitsPile**: keep the gate swap for Init / Destroy edges.
   Add explicit note: trashBitsPile is destroy-edge-only (no held-pose
   stream). The counter-decrement sync is a separate follow-up feature.

5. **PROBE sequence** before code-ship: probe-A + probe-D in a single
   gated build (`[probe] chippile_keys=1`), one hands-on test (host
   picks up an actorChipPile, observes log lines once). 5 LOC change,
   ~5 minute test.

