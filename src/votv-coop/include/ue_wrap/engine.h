// ue_wrap/engine.h -- high-level engine operations built on reflection.
//
// Engine-wrapper layer (principle 7): wraps specific engine UFunction calls
// behind a clean C++ API. No gameplay/network/coop state lives here -- just the
// marshaling to drive the engine through reflection::CallFunction (ProcessEvent).
//
// IMPORTANT: every call here invokes a UFunction, so it MUST run on the game
// thread (post via ue_wrap::game_thread::Post). Calling from another thread will
// crash the engine.

#pragma once

#include "ue_wrap/types.h"

namespace ue_wrap::engine {

// Run a console command (UKismetSystemLibrary::ExecuteConsoleCommand) -- the
// universal lever for "open <map>", "HighResShot ...", etc. Resolves the
// KismetSystemLibrary CDO + UFunction and a valid world context on first use
// (cached). Returns false if anything could not be resolved. Game thread only.
bool ExecuteConsoleCommand(const wchar_t* command);

// Spawn an actor of `actorClass` (a UClass*) at `location` via the deferred
// GameplayStatics pair (BeginDeferredActorSpawnFromClass + FinishSpawningActor)
// -- the same path the K2 SpawnActorFromClass node uses. Always spawns
// (CollisionHandlingOverride = AlwaysSpawn). Returns the spawned AActor* (a
// UObject*), or nullptr on failure. Game thread only.
void* SpawnActor(void* actorClass, const FVector& location);

// AActor::K2_GetActorLocation on `actor`. Returns (0,0,0) if it cannot be called.
FVector GetActorLocation(void* actor);

// AActor::K2_SetActorLocation on `actor` (teleport: bSweep=false, bTeleport=true
// -- snap to the absolute pose, the network pose-apply path). Returns the
// engine's success bool. Game thread only.
bool SetActorLocation(void* actor, const FVector& location);

}  // namespace ue_wrap::engine
