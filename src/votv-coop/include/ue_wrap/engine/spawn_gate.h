// ue_wrap/spawn_gate.h -- is the CURRENT world inside a spawn-refusal window?
//
// UWorld::SpawnActor silently returns null (Shipping strips the LogSpawn
// warning) while either bit is set on the world:
//   * bIsRunningConstructionScript -- set by AActor::ExecuteConstruction around
//     every BP actor's SCS+UCS construction. During a save-load's mass actor
//     construction (loadObjects/loadPrimitives) this covers most of the frame,
//     and nested ProcessEvent dispatches from inside those construction scripts
//     are exactly where the posted-task pump used to drain -- so every spawn a
//     task issued in that window failed (2026-07-04 join-window burst: 871
//     trash-proxy + 92 keyed-prop mirrors nulled in 2.5 s on the joining
//     client; the keyed mirrors were fire-and-forget = ghost-sync all session).
//   * bIsTearingDown -- the world is being destroyed; spawns are meaningless.
//
// WorldRefusesSpawns() reads those two bits through the SAME world-resolution
// path the engine's own K2 spawns take (GameInstance -> virtual GetWorld), so
// the game-thread pump can DEFER its drain to the next dispatch outside the
// window instead of running tasks that fire-and-fail (offsets: sdk_profile.h
// "UWorld spawn-refusal window"). No world / no GameInstance -> false: menu
// and boot tasks must keep draining.
//
// Game-thread only (the only caller is the ProcessEvent detour's drain gate).

#pragma once

namespace ue_wrap::spawn_gate {

// True while the current world refuses SpawnActor (construction-script window
// or world teardown). Cheap: one cached-liveness check + one virtual call +
// two byte reads; no allocation, no GUObjectArray walk on the steady path.
bool WorldRefusesSpawns();

}  // namespace ue_wrap::spawn_gate
