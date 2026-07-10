// coop/world/world_actor_detail.h -- INTERNAL (src-tree) shared header for
// the world_actor_sync module's two translation units:
//   world_actor_sync.cpp   -- HOST half + Install/lifecycle owner (interceptor,
//                             POST/destroy observers, pose stream, ex-enroll,
//                             connect snapshot; owns the session pointer, the
//                             Install-resolved spawn path and the bypass slot)
//   world_actor_mirror.cpp -- CLIENT half (wire materialize, wire destroy,
//                             pose apply + drive) -- the npc_mirror shape.
// Split 2026-07-05 (modular file-size rule; the audit-endorsed extraction after
// world_actor_sync.cpp reached 834 LOC at v102). NOT part of the public module
// API (that is include/coop/world/world_actor_sync.h); nothing outside
// these two TUs may include this.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::world_actor_sync::detail {

// The module's session pointer (atomic acquire-load; null when no session).
coop::net::Session* Session();

// The Install-resolved client-materialize path. All fields are written ONCE at
// Install (game thread) and read on the game thread only; null/-1 until
// resolved (the client receiver drops until then).
struct SpawnPath {
    void*   spawnFn;         // UGameplayStatics::BeginDeferredActorSpawnFromClass
    void*   finishSpawnFn;   // UGameplayStatics::FinishSpawningActor
    void*   gsCdo;           // the UGameplayStatics CDO (call target)
    int32_t returnParamOff;  // BeginDeferred's ReturnValue param offset
    void*   k2DestroyFn;     // AActor::K2_DestroyActor (mirror teardown)
};
SpawnPath GetSpawnPath();

// Bypass slot for wire-received WA spawns: the client materialize SETS the
// class immediately before its own BeginDeferred; the suppress interceptor
// (host TU) consumes it atomically and lets that one spawn through. Clear on
// a failed materialize so a stale class can't leak an allow-through.
void SetIncomingClass(void* cls);
void ClearIncomingClass();

// Wire-className trust gate (the kWorldActorAllowlist match on the wstring
// built from the wire string).
bool IsAllowlistedClassNameW(const std::wstring& nm);

}  // namespace coop::world_actor_sync::detail
