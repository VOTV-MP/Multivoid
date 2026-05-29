// coop/npc_sync.h -- Phase 5N1 NPC sync foundation.
//
// Engine-level interceptor on UGameplayStatics::BeginDeferredActorSpawnFromClass
// that:
//   - On HOST: optionally broadcasts EntitySpawn for allowlisted NPC classes
//     (gated by VOTVCOOP_NPC_SYNC=1 -- Inc2 ships the wire layer but the
//     client-side materializer is Inc3).
//   - On CLIENT: suppresses NPC spawns (allowlist matched by SuperStruct walk)
//     so only host-streamed NPCs exist on the client. Wire-received NPC
//     spawns bypass the suppressor via the MarkIncomingNpcSpawn slot.
//
// Idempotent install: Install() is called every NetPumpTick. The first call
// caches the session pointer + resolves the 12 NPC classes + the
// BeginDeferred UFunction. Once all are resolved the interceptor is set;
// further calls short-circuit.
//
// SPLIT (M-1 2026-05-29): the client-side receiver functions (OnEntitySpawn /
// OnEntityDestroy) live in `coop/npc_mirror.h` -- this file owns host-side
// PRE/POST/interceptor + resolution + lifecycle. The receivers consume the
// resolved UFunction refs through `npc_mirror::SetClientRefs(...)` (called
// once by Install when all refs become available) and access the shared
// allowlist + session pointer + bypass slot through the public accessors
// below.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::npc_sync {

// Cache the session pointer. Call once at boot, BEFORE the interceptor
// is installed (so the live interceptor reads through a stable pointer
// from its first fire). Mirrors prop_snapshot::SetSession.
void SetSession(coop::net::Session* session);

// Public read accessor for the cached session pointer. Used by the client
// receivers in coop::npc_mirror so they don't have to mirror a second
// atomic Session* (single source of truth, no sub-microsecond visible
// window between two stores).
coop::net::Session* GetSession();

// Try to install the NPC spawn interceptor. Resolves the GameplayStatics
// class + the BeginDeferredActorSpawnFromClass UFunction + the 12 NPC
// allowlist classes. Logs a warning + permanently disables if the engine
// UFunction is missing (build-incompatible). Caches the session pointer
// so the interceptor can read role()/connected()/SendEntitySpawn().
//
// `session` must outlive the install (the session_holder pattern -- the
// Session is created once at boot and lives for the process lifetime).
void Install(coop::net::Session* session);

// Bypass slot: the wire-received NPC spawn dispatcher (Inc3) will call
// this immediately before BeginDeferredActorSpawnFromClass to mark the
// next interceptor fire as "allow through, don't suppress". Single-shot;
// cleared on consume. Safe to call before Install (slot is just a static).
void MarkIncomingNpcSpawn(void* npcClass);

// Defensive clear of the bypass slot. Used by client receivers on error
// paths (ParamFrame invalid, BeginDeferred returned null, etc.) so a
// subsequent local spawn of the same class doesn't accidentally pass
// through and produce a rogue non-suppressed duplicate.
void ClearIncomingNpcSpawn();

// Trust-boundary check used by both host (interceptor) and client
// (receiver) sides: returns true iff `cls` is a UClass* that derives from
// any of the 12 allowlisted NPC bases (subclass-aware walk via
// UStruct.SuperStruct chain). Returns false if the allowlist isn't fully
// resolved yet (Install gates installation until all 12 bind).
bool IsAllowlistedClass(void* cls);

// Clear all per-session state: tracked-NPC map, sessionId counter, bypass
// slot. Called on net disconnect. Does NOT uninstall the interceptor (the
// 12 cached NPC classes + the UFunction pointer remain valid across
// disconnect; only the running-session bookkeeping resets).
void OnDisconnect();

}  // namespace coop::npc_sync
