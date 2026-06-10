# VOTV trash-clump MIRROR grab-lock RE — closing the drop-window dupe — 2026-06-08

## The exploit (recap, confirmed mechanism)

A CLIENT grabs a trash clump → carries a `Aprop_garbageClump_C` ball → on the HOST a
kinematic MIRROR of that ball is shown (driven into the collector's hands by the prop
pose stream). On DROP, the client converts its ball to a ground pile ~instantly, but on
the HOST the mirror ball sits on the ground as a **grabbable physics body for ~3 s**
until the client's `trash_collect_sync` death-watch broadcasts the authoritative landed
pile. In that window the HOST grabs the mirror ball → it becomes a real held object for
the host AND the client's pile still spawns → **one clump becomes two (DUPE)**.

The fix direction (a mirror is a puppet — never independently grabbable on the receiver)
is correct. This doc RE-proves the exact grab mechanism from BP bytecode and pins the
single minimal lock.

---

## 1. How a `garbageClump_C` ball is grabbed — VERIFIED: it's a QUERY sphere-trace + PHC

### 1.1 The clump exposes NO interaction/hold actions (so the action system can't grab it)

Disassembled from `research/bp_reflection/prop_garbageClump.json` (via
`research/bp_reflection/_disasm.py`, repak + kismet-analyzer of the cooked `.uasset`):

- `Aprop_garbageClump_C::canBeUsedHold` →
  ```
  [0] return = false
  [1] RETURN _
  ```
  **returns false.** (VERIFIED, _disasm.py output.)

- `Aprop_garbageClump_C::getActionOptions` → sets `options := arr[0]`,
  `options_enum := arr[0]`, `optionsNamesOverlay := arr[0]`, `number := b0`,
  `lookAtCenter := 0` then returns. **EMPTY action list — no pick-up/hold option is
  offered to the player's interaction UI.** (VERIFIED: every `Expression` is an
  `EX_ArrayConst` with `Elements: []`.)

- The whole BP grab-interface family is **ABSENT** on the clump (these exist on
  `Aprop_C` and `AactorChipPile_C`, but `Aprop_garbageClump_C` is a direct `AActor`
  subclass and does not override or inherit them as BP exports):
  `canBePickedUp` ABSENT, `canBeCollected` ABSENT, `canPickup` ABSENT,
  `playerTryToGrab` ABSENT, `playerTryToHold` ABSENT.
  (VERIFIED by scanning `prop_garbageClump.json` Exports.)

So the clump is **NOT** grabbed via the `getActionOptions` / `playerTryToHold` /
`canBeUsedHold` interaction path. (This also corrects the 2026-05-27 reconstruction
docs which guessed the clump rode `playerTryToHold` — it does not.)

### 1.2 The grab is the player's `useArm` QUERY sphere-trace → physics-handle light-grab

`AmainPlayer_C::useArm` (CXX dump `mainPlayer.hpp:468 bool useArm(FHitResult& OutHit)`),
disassembled:

```
[0] CALLVIRT arm(+0, CallFunc_arm_start, CallFunc_arm_end, CallFunc_arm_rotation)
[1] CallFunc_SphereTraceSingle_ReturnValue =
      SphereTraceSingle(self, arm_start, arm_end, Radius=1.0,
                        TraceChannel=b0, bTraceComplex=false, ActorsToIgnore=Temp_object,
                        DrawDebugType=b0, OutHit, bIgnoreSelf=true, ...)
[2] OutHit = CallFunc_SphereTraceSingle_OutHit
[3] ReturnValue = CallFunc_SphereTraceSingle_ReturnValue
```

- The `StackNode: -573` of statement [1] resolves to import **`SphereTraceSingle`**
  (= `UKismetSystemLibrary::SphereTraceSingle`). (VERIFIED: `imports[-(-573)-1].ObjectName
  == "SphereTraceSingle"`.)
- `SphereTraceSingle` is a **collision QUERY by trace channel** — it only returns a
  component whose `CollisionEnabled` includes **Query** and whose response on the trace
  channel (`ETraceTypeQuery` byte 0) is Block/Overlap. A component with collision set to
  **PhysicsOnly** has Query disabled entirely and is therefore **invisible to this
  trace**. (Stock UE4.27 `UKismetSystemLibrary` semantics.)
- The player's two carry paths BOTH start from this trace:
  - **Light grab (PHC):** `useArm` trace hit → `pickupObject(HitResult)` →
    `ExecuteUbergraph_mainPlayer(102953)` → drives the body via `grabHandle`
    (`UPhysicsHandleComponent @ mainPlayer.hpp:63 0x0688`) and stamps
    `grabbing_actor @0x07D0` / `grabbing_component @0x07D8`. This is the clump-ball path.
  - **Hold Object (heavy carry):** `AmainPlayer_C::"Hold Object"` is gated at stmt [16]
    `...->CALLVIRT canBePickedUp(...); POPFLOWIFNOT(...)` and `beginHoldingObject` — also
    reached only after the `useArm` trace identifies a target.

  There is **no grab path that bypasses the `useArm` Query trace.**

### 1.3 The clump does NOT self-configure collision; it relies on the mesh/CDO default

Scanning every `EX_CallMath`/function call across all `Aprop_garbageClump_C` BP exports
for collision/physics/attach/grab calls yields only `RetriggerableDelay`. The clump's
`ReceiveBeginPlay`/`Init` make **no** `SetCollisionEnabled` / `SetCollisionResponse` /
`SetSimulatePhysics` / `AttachToActor` calls. So a real clump ball is grabbable purely
because its `StaticMesh` body ships with the default grabbable collision
(**QueryAndPhysics**, simulating). (VERIFIED by call-name scan of `prop_garbageClump.json`.)

### 1.4 Why the HOST can grab the mirror today — the exact code site

The mirror's release handler `coop::remote_prop::OnRelease`
(`src/votv-coop/src/coop/remote_prop.cpp:495-509`, the non-`Aprop_C` clump branch where
`meshToActOn==null`) does, on the drop edge:

```cpp
ue_wrap::engine::SetActorRootCollisionEnabled(propActor, 3 /*QueryAndPhysics*/);   // line 504
ue_wrap::engine::SetActorSimulatePhysics(propActor, true);                          // line 505
ue_wrap::engine::SetActorRootPhysicsVelocity(propActor, lin, ang);                  // line 506
```

`3 = QueryAndPhysics` re-enables the **Query** channel on the dropped mirror's root
(`StaticMesh`, which is the clump root — see §3) → the host's `useArm` SphereTrace hits
it → host grabs it → dupe. (VERIFIED: file+line read.)

---

## 2. The ONE recommended lock — drop Query, keep Physics, on the clump release

**Change `remote_prop.cpp:504` from `3 /*QueryAndPhysics*/` to `2 /*PhysicsOnly*/`.**

- Exact call: `ue_wrap::engine::SetActorRootCollisionEnabled(propActor, 2 /*PhysicsOnly*/);`
- Enum: `ECollisionEnabled::Type`: `0=NoCollision, 1=QueryOnly, 2=PhysicsOnly,
  3=QueryAndPhysics`. The wrapper already documents + accepts these
  (`engine_attach.cpp:89-100`, `SetCollisionEnabled` UFunction, `NewType` param).
- Underlying UFunction: `UPrimitiveComponent::SetCollisionEnabled(NewType)`, dispatched on
  the actor's root primitive via `K2_GetRootComponent` (the clump's `StaticMesh`).

### Why this is the correct minimal lock (and beats the alternatives)

- **It blocks every grab path.** Both the PHC light-grab and the heavy Hold-Object path
  begin with the `useArm` **Query** sphere-trace (§1.2). PhysicsOnly removes the root from
  ALL collision QUERIES → the trace cannot return the mirror → it can never become a
  grab/hold target. Root-cause fix at the actual gate (the Query channel the grab uses),
  not a broad suppression.
- **It does NOT break the resting visual.** PhysicsOnly keeps the body in the PhysX
  simulation. With `SetActorSimulatePhysics(true)` still called on line 505, the dropped
  mirror still free-falls, collides with the world's physics bodies, lands, and rests
  exactly as before — only its *query* visibility is removed. (Query vs Physics are
  independent collision facets in UE; traces/overlaps use Query, rigid-body contacts use
  Physics.)
- **It does NOT break the held visual.** While held, the clump mirror is driven
  KINEMATICALLY by `SetActorLocation` each pose tick and is NOT simulating: at spawn,
  `remote_prop_spawn.cpp:481-493` operates on `mesh = GetStaticMesh(spawned)` which is
  **null** for the non-Aprop_C clump, so no physics/collision is set on the clump root at
  spawn (only `SetActorRootNotifyRigidBodyCollision(false)` at line 442). `OnRelease` is
  the FIRST and ONLY place the clump root's collision is set — so the change touches only
  the post-drop state, never the in-hand follow.

### Rejected alternatives

- **(a) `SetActorEnableCollision(false)` / `NoCollision`** — would make the dropped mirror
  fall THROUGH the floor (no physics contact), breaking the "resting on the ground while
  it waits to convert" visual the user wants. Rejected.
- **(c) a BP non-grabbable flag** — there is none usable: `canConvert@0x0248` /
  `holdPlayer@0x0240` don't gate the grab (the grab is a Query trace on the player side,
  not a clump-BP check), and `canBeUsedHold`/`getActionOptions` are already false/empty yet
  the clump is still grabbed via the PHC trace. No BP field can block the `useArm` trace.
  Rejected — the Query collision facet is the real gate.

### ue_wrap helper needed?

**NONE.** `ue_wrap::engine::SetActorRootCollisionEnabled(void* actor, uint8_t
collisionType)` already exists and already accepts `2 = PhysicsOnly`
(`include/ue_wrap/engine.h:671`, impl `src/ue_wrap/engine_attach.cpp:89`). The fix is a
single literal change at one call site. (The doc comment at `engine.h:668-670` /
`engine_attach.cpp:91-92` that says "needs 3 so it … lands" should be updated to note
PhysicsOnly is used to keep landing while denying the grab trace — comment only.)

---

## 3. The clump root IS its `StaticMesh` (so the lock targets the grabbed body)

The wrapper acts on `K2_GetRootComponent`. Two independent proofs the clump root is the
`StaticMesh` (the body the grab trace would hit and the body that self-converts):

- The clump's ground-hit→self-convert handler is
  `BndEvt__prop_garbageClump_StaticMesh_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature`
  → bound to **`StaticMesh`**'s `ComponentHit`. The existing dup-fix already silences it
  via `SetActorRootNotifyRigidBodyCollision(false)` (root-targeted) and it works — so the
  root == StaticMesh. (VERIFIED: handler name + `remote_prop_spawn.cpp:442` comment/behaviour.)
- The existing release physics (`SetActorRootCollisionEnabled(…,3)` +
  `SetActorSimulatePhysics(true)` + velocity, root-targeted) already makes the mirror land
  correctly today — only possible if the root is the simulating mesh body.

So `SetActorRootCollisionEnabled(propActor, 2)` sets PhysicsOnly on exactly the body the
`useArm` SphereTrace would otherwise hit. (No new "set collision on the StaticMesh by
offset" helper is needed — root == StaticMesh.)

---

## 4. The converted ground PILE stays grabbable — VERIFIED (separate path, not affected)

The authoritative landed pile is spawned by `trash_collect_sync::BroadcastLandedPileNear`
(`kFreshLanded` flag) → `remote_prop_spawn::OnSpawn`, a DIFFERENT code path:

- It runs the pile through `DriveSimulate(GetStaticMesh(pile), sim)` +
  `TurnChipPileToPile(...)`; it **never** calls `OnRelease`, so it never hits the
  `SetActorRootCollisionEnabled(propActor, …)` clump line. (VERIFIED: `OnSpawn` body
  `remote_prop_spawn.cpp:466-493`; the clump-collision line lives only in `OnRelease`.)
- `OnSpawn`'s only collision touch is `RestoreCollisionIfNeeded(...)`, which is a no-op
  unless the class is in `IsCollisionRestoreClass` — currently **only the cap mushroom**;
  `AactorChipPile_C`/pile is NOT listed. So the pile keeps its **BP-default
  QueryAndPhysics** collision and is fully grabbable by both peers.
  (VERIFIED: `remote_prop_spawn.cpp:83-128, 502`.)

`AactorChipPile_C` (the pile) is also the class that legitimately carries the grab
interface (`canBePickedUp`, `canBeCollected`, `playerTryToGrab`, `playerTryToCollect`,
`canPickup`, `getActionOptions`, plus a dedicated `Collision` UStaticMeshComponent @0x0230)
— so once landed it is grabbable through the normal `useArm` Query trace, as intended.
(VERIFIED: `actorChipPile.hpp:45-64`.)

The lock therefore applies ONLY to the in-flight/resting clump-ball MIRROR (the
`OnRelease` non-Aprop_C branch) and leaves the landed pile grabbable. No accidental
inheritance.

---

## 5. Summary

- **Grab mechanism:** the player's `AmainPlayer_C::useArm` does a
  `UKismetSystemLibrary::SphereTraceSingle` (a **collision QUERY**, trace channel byte 0)
  to find a target, then PHC-light-grabs (or heavy-holds) the simulating body it hits. The
  `Aprop_garbageClump_C` ball has NO BP grab/hold actions (`canBeUsedHold=false`,
  `getActionOptions=[]`, no `canBePickedUp`/`playerTryToHold`) — it is grabbable ONLY
  because its `StaticMesh` root ships with **QueryAndPhysics** collision and the trace can
  see it.
- **The dupe** is that `remote_prop::OnRelease` re-enables **QueryAndPhysics** (`3`) on the
  dropped mirror (`remote_prop.cpp:504`), so the host's grab trace can hit it before the
  authoritative pile arrives.
- **The fix (one value):** `remote_prop.cpp:504` → `SetActorRootCollisionEnabled(propActor,
  2 /*PhysicsOnly*/)`. Keeps physical landing/resting (PhysX contacts) + the kinematic
  held-follow (untouched — held phase never sets collision), drops the **Query** facet so
  the `useArm` SphereTrace misses the mirror → not grabbable → no dupe.
- **ue_wrap helper:** none needed; `SetActorRootCollisionEnabled` already accepts
  `2 = PhysicsOnly`.
- **Landed pile:** spawned via the separate `OnSpawn`/`kFreshLanded` path that never runs
  the clump collision line and keeps BP-default QueryAndPhysics → stays grabbable by both
  peers. Confirmed isolated.

### Files
- Fix site: `src/votv-coop/src/coop/remote_prop.cpp:504` (change `3` → `2`).
- Wrapper (already sufficient): `include/ue_wrap/engine.h:671`,
  `src/ue_wrap/engine_attach.cpp:89-100` (update the "needs 3 so it lands" comment).
- Grab-RE sources: `research/bp_reflection/mainPlayer.json` (`useArm`, `Hold Object`,
  `pickupObject`), `research/bp_reflection/prop_garbageClump.json` (`canBeUsedHold`,
  `getActionOptions`, the StaticMesh `ComponentHit` BndEvt).
- CXX dumps: `mainPlayer.hpp` (grabHandle@0x0688, grabbing_actor@0x07D0, useArm),
  `prop_garbageClump.hpp` (no grab interface; StaticMesh@0x0230, holdPlayer@0x0240),
  `actorChipPile.hpp` (full grab interface + Collision@0x0230 — the grabbable landed pile).
