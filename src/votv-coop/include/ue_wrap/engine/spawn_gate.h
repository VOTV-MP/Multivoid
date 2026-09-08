// ue_wrap/engine/spawn_gate.h -- is the CURRENT world inside a spawn-refusal window?
//
// UWorld::SpawnActor silently returns null (a Shipping build strips the LogSpawn warning) while
// either bit is set on the world:
//   * bIsRunningConstructionScript -- set by AActor::ExecuteConstruction around every blueprint
//     actor's construction. A save-load's mass construction covers most of a frame, and the nested
//     ProcessEvent dispatches from inside those scripts are where the posted-task pump used to
//     drain, so every spawn a task issued there failed: one join window nulled 871 trash proxies
//     and 92 keyed-prop mirrors in 2.5 s, and the keyed ones stayed missing all session.
//   * bIsTearingDown -- the world is being destroyed; spawns are meaningless.
//
// WorldRefusesSpawns() reads those bits through the SAME world-resolution path the engine's own K2
// spawns take (GameInstance -> virtual GetWorld), so the game-thread pump can DEFER its drain past
// the window rather than run tasks that fire and fail. No world or no GameInstance answers false:
// menu and boot tasks must keep draining.

#pragma once

namespace ue_wrap::spawn_gate {

// True while the current world refuses SpawnActor (the construction-script window or world
// teardown). Game thread only -- the sole caller is the ProcessEvent detour's drain gate. Cheap:
// one cached-liveness check, one virtual call and two byte reads; no allocation, and no
// GUObjectArray walk on the steady path.
bool WorldRefusesSpawns();

}  // namespace ue_wrap::spawn_gate
