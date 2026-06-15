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

#include "coop/element/element.h"  // ElementId (GetNpcIdForActor)

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

// True once Install()'s attempt has completed (it stops retrying). On the
// HAPPY path this means the subclass-aware allowlist (IsAllowlistedClass) is
// FULLY resolved -- a partial resolve early-returns WITHOUT latching, so a
// latched-true install that resolved the allowlist resolved ALL of it -- and
// the host lifecycle path may still be disabled (g_npcSyncDisabledThisProcess)
// without affecting that. On a BROKEN build (BeginDeferred UFunction / its
// params unresolvable) Install latches true with a NULL allowlist to stop
// retrying; IsInstalled() is then true with NOTHING resolved. That is safe for
// the only caller (the client reconcile sweep): IsAllowlistedClass returns
// false for unresolved slots, so the sweep classifies nothing and destroys
// nothing -- a benign no-op on a build where NPC sync is dead anyway. A caller
// MUST NOT gate broader world-ready logic on the interceptor being live
// (NPC-class load timing must never block prop/door replay). Used by net_pump
// to time the client-NPC reconcile sweep (npc_mirror::DestroyUntrackedClient-
// Npcs). Game thread.
bool IsInstalled();

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

// Reverse lookup: live (or just-destroyed) NPC actor -> ElementId, from the
// same map the K2_DestroyActor PRE gates on. kInvalidId when untracked.
// Mutex-guarded; safe from any thread (coop::kerfur_convert reads it inside
// a ProcessEvent interceptor on a parallel-anim worker).
coop::element::ElementId GetNpcIdForActor(void* actor);

// Explicit destroy-sync for an NPC actor whose K2_DestroyActor our PRE
// observer could NOT see (BP-internal by-name call -- kerfurOmega_C::
// dropKerfurProp, v67). Exactly the PRE body: erase the reverse-map entry,
// drain the Element (deferred), broadcast EntityDestroy. `actor` is used as
// a map key only (never dereferenced) -- callable on a PendingKill/purged
// pointer; no-ops when the actor is untracked (double-call safe).
void SyncDestroyedNpcActor(void* actor);

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

// HOST-only connect-snapshot (2026-06-04, user: "existing npcs mirrored when a fresh peer
// joins"). Re-send an EntitySpawn (class + CURRENT world transform) for every already-spawned
// Npc Element to the freshly-connected client `peerSlot`, so a joiner materializes mirrors of
// NPCs that spawned BEFORE it joined (the spawn-time EntitySpawn only reaches peers connected
// at spawn time). The client's existing npc_mirror::OnEntitySpawn path materializes each;
// MirrorManager::Install is idempotent so a re-send to an already-mirroring peer is a no-op.
// Reads each Element's bound actor for the current transform (skips elements whose actor is
// not yet bound). Net-pump connect edge. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// HOST-only PRE-EXISTING world-NPC enumeration (2026-06-07, user "two kerfurs on host" -- a kerfur
// already in the loaded save wasn't synced). Walk GUObjectArray for allowlisted-NPC actors that
// EXIST but are NOT yet coop-tracked -- i.e. loaded with the level BEFORE the interceptor installed,
// so they never went through the intercepted BeginDeferred and were never registered. Register each
// as a host Npc Element (alloc + bind the live actor + reverse-map) -- the same end state the
// interceptor+POST observer reach for a fresh spawn, but for an actor that loaded with the level.
// QueueConnectBroadcastForSlot then mirrors them to the joiner + TickPoseStream streams them after.
// Idempotent (skips already-tracked actors). Called at the host connect edge BEFORE the connect-
// snapshot. Returns the count newly registered. Game thread. Cold path: one GUObjectArray walk per
// call (connect-rare), like the prop world-snapshot. Soft-cap note: npc_sync.cpp is at ~800 LOC;
// this host-side function is a candidate to extract alongside QueueConnectBroadcastForSlot/
// TickPoseStream into a host-egress TU if npc_sync.cpp grows further.
int RegisterExistingWorldNpcs();

// HOST-only per-tick NPC pose stream driver: read each live Npc Element's current world
// transform + publish the batch to the session for the net thread to fan out (EntityPose,
// unreliable). Makes the client mirrors MOVE (they otherwise sit at spawn). Cheap no-op off
// the host / when there are no NPCs. Call every net-pump tick. Game thread.
void TickPoseStream();

// Resolved GameplayStatics spawn refs (valid only AFTER Install completes +
// the 12 NPC classes resolve). Exposed so the dev-spawn tool
// (coop::dev::spawn_npc) can drive a BeginDeferredActorSpawnFromClass +
// FinishSpawningActor host NPC spawn through the SAME UFunction the interceptor
// hooks -- so the host alloc+broadcast path (AllocAndInstall + EntitySpawn) runs
// exactly as a real spawn would. Returns false if not yet installed/resolved.
//
// VALIDATED 2026-06-07 (npctest --peers 2): host dev-spawn + EntitySpawn
// broadcast + the host POST observer BINDING the actor (log "[host POST]: bound
// actor", POST-bound=1) + CLIENT mirror Install all work end-to-end, mirror at
// the host's exact spawn loc. host-side NPC DESTROY-sync IS now exercised by a
// dev-spawn (the bound actor is in g_actorToNpcId, so K2_DestroyActor PRE fires).
//
// HISTORY (resolved): a 2026-05-30 12:58 note here claimed NpcSpawn_POST did NOT
// fire for a reflection-initiated BeginDeferred. That was the dual-POST-observer
// CLOBBER, not a reflection-vs-engine difference: npc_sync's NpcSpawn_POST AND
// weather_lightning's OnSpawnPostLightning both target BeginDeferred, and the old
// one-cb-per-UFunction observer table silently OVERWROTE npc_sync's slot. Commit
// eb3282d (multi-observer-per-UFunction, 2026-05-30 16:58 -- 4h after that note)
// gave each (target, cb) its own slot so both fire; the note was never re-checked
// until the 2026-06-07 npctest above proved POST-bound=1.
struct DevSpawnRefs {
    void* beginDeferredFn = nullptr;  // GameplayStatics.BeginDeferredActorSpawnFromClass
    void* finishSpawnFn   = nullptr;  // GameplayStatics.FinishSpawningActor
    void* gsCdo           = nullptr;  // GameplayStatics CDO (the call self)
};
bool GetDevSpawnRefs(DevSpawnRefs& out);

}  // namespace coop::npc_sync
