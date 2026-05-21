# Finding: VOTV player class, controller, GameMode, save + spawn mechanism

**Date**: 2026-05-21
**Game version**: Alpha 0.9.0-n
**Phase**: 1.1 / 1.2 / 1.7 + Phase 2.1 mechanism
**Source**: UE4SS reflection dumps (CXX headers + object dump), in-world
save loaded (map `untitled_1`). Authoritative for this build.
**Status**: core class hierarchy mapped; spawn path identified and already
runnable in-game. Exhaustive per-field offset mapping deferred to Phase 4
(replication) when specific fields are needed.

All offsets below are from `CXXHeaderDump/*.hpp` with `DumpOffsetsAndSizes=1`.

## Player controller — stock `APlayerController` (1.3 anchor)

The player is driven by an **unmodified `APlayerController`** (no BP
subclass), instance `PersistentLevel.PlayerController_2147468473` in map
`untitled_1`. Input flows through the engine's standard
`APlayerController` → `UInputComponent` path; VOTV adds input via the
pawn's `InpActEvt_*` Blueprint input-action events (e.g.
`InpActEvt_inventory_*` in `mainPlayer.hpp`).

Implication for coop: P2's controller is also a stock `APlayerController`
(see spawn mechanism below); we route P2 input into it rather than
subclassing.

## Player pawn — `AmainPlayer_C : public ACharacter` (1.2)

`CXXHeaderDump/mainPlayer.hpp`, ~825 members. Pose/velocity/orientation
are inherited from `ACharacter` (RootComponent transform +
`UCharacterMovementComponent`), confirmed by SplitScreenMod reading
`Pawn.RootComponent:K2_GetComponentLocation/Rotation`. VOTV-specific state:

| Field | Offset | Meaning |
|---|---|---|
| `UCameraComponent* Camera` | 0x0530 | first-person camera |
| `UArrowComponent* cameraRoot` | 0x0538 | camera pivot |
| `USpringArmComponent* cameraLag` | 0x04D0 | camera lag |
| `UPhysicsHandleComponent* grabHandle` | 0x0688 | light-object grab |
| `UPhysicsConstraintComponent* heavyGrab` | 0x04F0 | heavy-object grab |
| `AActor* grabbing_actor` | 0x07D0 | currently grabbed actor |
| `UPrimitiveComponent* grabbing_component` | 0x07D8 | grabbed component |
| `AActor* holding_actor` | 0x0A20 | held prop |
| `FName holding_name` | 0x0A30 | held prop name |
| `bool input_crouch` | 0x09BC | crouch input latch |
| `float foodDraining` | 0x0B80 | hunger drain rate |
| `float sleepDraining` | 0x0B84 | sleep drain rate |
| `float sleepComfort` | 0x0D98 | sleep comfort |
| `bool equipped_emf / geiger / metalDetec / krampushat` | 0x0CDC / 0x0CE4 / 0x0CED / 0x0CEE | equipped tool flags |
| `UAnimBlueprint_*_C* animInst_playerView` | 0x0E80 | view anim instance |

Relevant UFunctions (BP events) on the pawn: `beginHoldingObject`,
`throwHoldingProp`, `putObjectInventory2`, `playerTryToGrab`, `addFood`
(food/sleep/health), `traceThrow`, `StopMovement`, `fixCrouch`. There is a
separate `playerSleepingPawn_C` for the sleeping state (pawn swap, not a
mode flag).

Note: persistent stat *values* (food/sleep/health amounts) are not stored
as plainly-named floats on the pawn — only drain *rates* are here. They
likely live on `save_main_C` / the GameMode; pin exact storage in Phase 4
when replicating stats.

## GameMode — `AmainGamemode_C : public AGameModeBase`

`CXXHeaderDump/mainGamemode.hpp`. Owns the whole signal/base-sim loop and
several coop-relevant authoritative singletons:

- `APawn* origPawn` (0x04F8) — reference to the original player pawn.
- Save refs: `UsaveSlot_C* saveSlot` (0x04B0), `Usave_main_C* save_main`
  (0x0508), `UmainGameInstance_C* GameInstance` (0x0510).
- World sim: `dishs` (TArray<Adish_C*>), `activeDishes`, `savedSignals`,
  power model (`totalPower`/`usedPower`/`power_*`), `servers`,
  `daynightCycle` (`AdaynightCycle_C`), `powerControl`, `ambMaster`.
- Spawning helpers: `spawnPropThroughGamemode(name, transform, amount,
  out actor)` and many world-event spawners.

This is the host-authoritative state hub for Phase 4.3/4.5/4.6 (world,
save, progression). The day/night cycle and power are single global values
→ host-authoritative scalars.

## Save system (1.7)

Classes present: `save_main.hpp` (`Usave_main_C`), `saveSlot.hpp`
(`UsaveSlot_C`), `mainGameInstance.hpp` (`UmainGameInstance_C`), plus UI
(`ui_saveSlots`, `uicomp_saveSlot*`). UE `USaveGame`-derived. Host-
authoritative save reuse is viable; detailed layout + load entry UFunction
to be mapped when Phase 4.5 needs it.

## Entity factory + spawn mechanism (1.1 + Phase 2.1 derisk)

**Goal is networked LAN coop, NOT split-screen.** The remote player is a
pawn in the local full-screen world driven by network input/pose — not a
local split-screen player.

- **Shipping primitive — `UWorld::SpawnActor<AmainPlayer_C>`**: spawn the
  remote pawn directly. It brings VOTV's rendering/anim/physics/grab for
  free. We wrap it in `coop::RemotePlayer` (parallel class hierarchy,
  principle 3) and drive it from network input / interpolated pose. No
  `ULocalPlayer`, no local gamepad binding, no viewport split. A
  controller is attached only insofar as VOTV systems require a possessor;
  it is OUR controller, not a local player.
- **Do NOT use `UGameplayStatics::CreatePlayer` for the remote player.** It
  creates a `ULocalPlayer` tied to local input + the engine's split-screen
  viewport path — local-coop baggage that does not fit a networked remote.

### SplitScreenMod = reference only (not the architecture)

The bundled `SplitScreenMod` (`Mods/SplitScreenMod/Scripts/main.lua`,
CTRL+Y/U/I) uses `CreatePlayer`. Its value to us is narrow: it **proves
VOTV tolerates a 2nd `AmainPlayer_C` instance in the world** and is a handy
code sample of the UE4SS Lua API (`GetGameplayStatics()`, `FindAllOf`,
`ExecuteInGameThread`, pawn transform getters). We do **not** ship its
mechanism. Optionally it can serve as a one-off crash smoke-test ("does a
2nd pawn tick at all?"), but the real Phase 2.1 work spawns via
`SpawnActor` on our own path.

**Implication**: Phase 2.1 ("can a 2nd `AmainPlayer_C` exist and tick?") is
de-risked in principle (the engine supports multiple pawns; a mod already
instantiates one). Remaining Phase 2 = the SP-only-assumption crash hunt
(principle 4, per-site fixes) on our `SpawnActor` orphan, plus confirming
it survives VOTV singletons (camera/grab/inventory/save) that may assume
one player.

## Next steps

1. **Build the orphan on our path**: `SpawnActor<AmainPlayer_C>` from a
   minimal UE4SS probe (Lua first, then the C++ mod), attach a controller
   we own, confirm it ticks; crash-hunt per-site. (`SplitScreenMod` CTRL+Y
   is an optional smoke-test only — not the implementation.)
2. Map `save_main_C` layout + the load entry UFunction (1.7 detail).
3. Confirm the input-action path on `AmainPlayer_C` for input replication
   (4.1) — enumerate `InpActEvt_*` events.
4. Identify single-instance enforcement for same-machine dual-launch (0.8).
