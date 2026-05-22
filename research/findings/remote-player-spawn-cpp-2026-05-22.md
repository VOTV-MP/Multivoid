# coop::RemotePlayer — orphan spawn + pose-drive in standalone C++ — 2026-05-22

**Goal**: port the validated Phase 2.1 orphan spawn into C++ behind
`coop::RemotePlayer` — spawn a 2nd `mainPlayer_C` and pose-drive it, all via our
own `CallFunction`/ProcessEvent on the game-thread context, standalone (no
UE4SS). This is the engine-side primitive the network layer will drive (Phase 3).

It required (and delivered) **generic UFunction parameter marshaling**: read each
param's byte offset from the live UFunction's FProperty chain instead of
hardcoding — correct-by-construction and version-portable.

## Result — PROVEN LIVE, standalone, no crash

```
skip-to-gameplay -> world=Untitled_1, mainPlayer_C count=1
SpawnActor -> 0x...625E20E0 (finished same) at (-37495,69978,6420)
RemotePlayer::Spawn: 2nd mainPlayer_C spawned
report [post-spawn]: mainPlayer_C count=2          <- orphan exists, no clobber
drive step 1 set X=-37345 ok=1 -> read (-37345, 69978, 6420)
drive step 2 set X=-37195 ok=1 -> read (-37195, 69978, 6442)
...
drive step 5 set X=-36745 ok=1 -> read (-36745, 69999, 6460)
report [post-drive soak]: mainPlayer_C count=2     <- stable, no crash
```

- Spawn worked via the BlueprintCallable deferred pair; the actor is valid and
  queryable. Count 1 -> 2, local player kept its pawn (no clobber) — matches the
  Lua Phase 2.1 result, now in standalone C++.
- **Pose-drive by absolute teleport works**: `K2_SetActorLocation(bSweep=false,
  bTeleport=true)` snaps the orphan to each set position; the read-back tracks
  the set X exactly. This is the network pose-apply path — and it is *better*
  than the Lua harness's `bSweep=true`, which got collision-blocked after one
  step. (Z drifts a little as the character capsule settles — expected.)
- No crash through spawn + 5 drives + soak.

## Generic UFunction parameter marshaling (the enabling capability)

Decompiling shipping `UObject::ProcessEvent` (rva 0x1465930) gave the UE4.27
layout used to marshal ANY UFunction (`sdk_profile::off`):

| field | offset | from ProcessEvent |
|---|---|---|
| UStruct::ChildProperties (FField*, params first) | 0x50 | param-walk head |
| UStruct::PropertiesSize (int32, frame size)      | 0x58 | alloca size |
| UFunction::ParmsSize (uint16)                     | 0xB6 | params memcpy size |
| UFunction::ReturnValueOffset (uint16)             | 0xB8 | return slot |
| FField::Next (FField*)                            | 0x20 | chain walk |
| FField::NamePrivate (FName)                       | 0x28 | (FField, not UObject@0x18) |
| FProperty::ElementSize (int32)                    | 0x38 | size = Elem*ArrayDim |
| FProperty::ArrayDim (int32)                       | 0x3C | |
| FProperty::PropertyFlags (uint64)                 | 0x40 | &0x80 CPF_Parm, &0x100 OutParm, &0x400 Return |
| FProperty::Offset_Internal (int32)                | 0x4C | byte offset in frame |

`reflection::FunctionParams(fn)` walks ChildProperties via Next, collects
CPF_Parm properties with {name, offset, size, flags}. `FunctionFrameSize` =
PropertiesSize. `FindParamOffset(fn, name)` -> byte offset.

**Validated live** against three known signatures (the param dump matched UE4.27
exactly — names, offsets, sizes, OUT/RET flags):
- `Actor::K2_SetActorLocation` (frame 156): NewLocation@0x00(12), bSweep@0x0c,
  SweepHitResult@0x10(136,OUT), bTeleport@0x98, ReturnValue@0x99(OUT RET).
- `GameplayStatics::BeginDeferredActorSpawnFromClass` (frame 96):
  WorldContextObject@0x00, ActorClass@0x08, SpawnTransform@0x10(48),
  CollisionHandlingOverride@0x40, Owner@0x48, ReturnValue@0x50(8,RET).
- `GameplayStatics::FinishSpawningActor` (frame 80): Actor@0x00,
  SpawnTransform@0x10(48), ReturnValue@0x40(8,RET).

`ue_wrap::ParamFrame` (call.h/.cpp): allocates a zeroed frame of FrameSize, sets
args by name (offset from reflection), reads OUT/return params back after the
call. `ue_wrap::FVector`/`FTransform` PODs (types.h) match the binary layout
(FTransform=48: FQuat + Translation + Scale3D).

## Files

- `ue_wrap/sdk_profile.h` — FProperty/UStruct offsets (`off::`), `cpf::` flags,
  GameplayStatics/Actor spawn+transform function names.
- `ue_wrap/reflection.{h,cpp}` — `FunctionParams`, `FunctionFrameSize`,
  `FindParamOffset`, `CountObjectsByClass`.
- `ue_wrap/types.h` — `FVector`, `FTransform`, `MakeTransform`.
- `ue_wrap/call.{h,cpp}` — `ParamFrame` + `Call`.
- `ue_wrap/engine.{h,cpp}` — `SpawnActor` (deferred pair, AlwaysSpawn),
  `GetActorLocation`, `SetActorLocation` (teleport).
- `coop/remote_player.{h,cpp}` — `RemotePlayer`: Spawn / SetLocation / GetLocation
  (holds the engine actor pointer; principle 3 parallel hierarchy).
- `harness/harness.cpp` — `orphan` scenario (spawn + count + 5 pose-drives + soak)
  + temporary `DumpParams` validator.

## Significance

This is the substrate for ALL future entity/object sync (user note, see
`docs/COOP_SCOPE.md` architectural note): we can now call any UFunction on any
object with correct param marshaling. The sibling capability still to build is
generic **UProperty get/set** (read/write a named field — same FProperty
`Offset_Internal`), which is what state replication needs.

## Lua coopTestHarness retirement (RULE No.2)

The C++ harness now supersedes the Lua mod for: newgame/skip-to-gameplay,
screenshot, report, and **orphan spawn/drive**. NOT yet ported: save load
(`load:<slot>`), widget inspect, singleton check. The Lua harness stays until
those are ported (removing them now would drop a still-needed capability, not a
replaced one). Plan: port save-load + inspect, then delete the Lua harness.

## Next

- Phase 3: UDP transport -> drive `RemotePlayer::SetLocation` from received
  snapshots (host-authoritative). Auto-spawn on first packet.
- Generic UProperty get/set in `ue_wrap` (for entity state replication, 4.3).
