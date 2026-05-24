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

#pragma once

namespace coop::net { class Session; }

namespace coop::npc_sync {

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

// Clear all per-session state: tracked-NPC map, sessionId counter, bypass
// slot. Called on net disconnect. Does NOT uninstall the interceptor (the
// 12 cached NPC classes + the UFunction pointer remain valid across
// disconnect; only the running-session bookkeeping resets).
void OnDisconnect();

}  // namespace coop::npc_sync
