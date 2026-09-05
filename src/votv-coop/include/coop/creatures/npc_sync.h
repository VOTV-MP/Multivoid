// coop/creatures/npc_sync.h -- the NPC sync foundation: an interceptor on the engine's
// deferred-spawn function that, on the host, broadcasts an entity spawn for allowlisted NPC
// classes and, on a client, suppresses NPC spawns (the allowlist matched by a SuperStruct
// walk) so only host-streamed NPCs exist there; a wire-received spawn bypasses the suppressor
// through the incoming-spawn slot. The install is idempotent and called every pump tick: the
// first call caches the session pointer and resolves the NPC classes and the function, and
// once all are resolved the interceptor is set. The client-side receivers live in npc_mirror.h
// and consume the resolved function references through SetClientRefs, plus the allowlist, the
// session pointer and the bypass slot through the accessors here; this file owns the host-side
// observers, the interceptor, resolution and the lifecycle.

#pragma once

#include "coop/element/element.h"  // ElementId (GetNpcIdForActor)

#include <string>  // RegisterHostNpcSilent className

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::npc_sync {

// Cache the session pointer, once at boot, before the interceptor is installed, so the live
// interceptor reads through a stable pointer from its first fire.
void SetSession(coop::net::Session* session);

// The cached session pointer, for the client receivers, so they do not mirror a second atomic.
coop::net::Session* GetSession();

// Try to install the NPC spawn interceptor: resolves the gameplay-statics class, the
// deferred-spawn function and the allowlisted NPC classes; logs and permanently disables if
// the engine function is missing. `session` must outlive the install; it lives for the
// process.
void Install(coop::net::Session* session);

// True once the install's attempt has completed and stopped retrying. On the happy path the
// subclass-aware allowlist is fully resolved (a partial resolve early-returns without
// latching), while the host lifecycle may still be disabled separately. On a broken build (the
// deferred-spawn function or its params unresolvable) it latches true with a null allowlist to
// stop retrying, which is safe for its one caller, the client reconcile sweep: the allowlist
// test returns false for unresolved slots, so the sweep classifies nothing and destroys
// nothing. Broader world-ready logic must never gate on the interceptor being live; NPC class
// load timing must not block prop or door replay. Game thread.
bool IsInstalled();

// True iff the install permanently disabled the host NPC lifecycle this process (the observer
// table was full, so no guaranteed POST or destroy observer). The world enumeration gates on
// this and IsInstalled before allocating an Npc Element, the same gate the interceptor uses,
// so it never leaks an Element no destroy observer can close. Atomic; any thread.
bool IsHostNpcSyncDisabled();

// The bypass slot: the wire-received spawn dispatcher calls this immediately before the
// deferred spawn to mark the next interceptor fire as allow-through. Single-shot, cleared on
// consume; safe before the install.
void MarkIncomingNpcSpawn(void* npcClass);

// A defensive clear of the bypass slot, for the client receivers' error paths (an invalid
// frame, a null spawn), so a later local spawn of the same class does not pass through as a
// rogue duplicate.
void ClearIncomingNpcSpawn();

// The reverse lookup, a live or just-destroyed NPC actor to its eid, from the map the destroy
// observer gates on; kInvalidId when untracked. Mutex-guarded, any thread (kerfur_convert
// reads it inside an interceptor on a parallel-anim worker).
coop::element::ElementId GetNpcIdForActor(void* actor);

// Insert or overwrite the reverse-map entry GetNpcIdForActor reads. The world enumeration
// publishes a pre-existing NPC it registered into the same host map the observers use, so the
// destroy observer closes its lifecycle like a fresh spawn's. Mutex-guarded; game thread. A
// no-op for a null actor.
void MapActorToNpcId(void* actor, coop::element::ElementId eid);

// The explicit destroy sync for an NPC actor whose K2_DestroyActor the PRE observer cannot see
// (a blueprint-internal call by name, as the kerfur's turn-off is): the PRE body exactly, the
// reverse-map erase, the deferred drain and the destroy broadcast. The actor is a map key
// only, never dereferenced, so a purged pointer is fine; a no-op when untracked.
void SyncDestroyedNpcActor(void* actor);

// The eid-keyed variant for the pose-walk dead-actor retire (a self-destroy the PRE never
// sees). Keying by eid rather than through the actor map means a same-address slot rebind can
// never retire a different NPC: the map entry is erased only if it still maps to this eid. The
// same teardown and broadcast as the PRE; the actor is a map key only. Double-call safe. Game
// thread.
void SyncDestroyedNpcByEid(coop::element::ElementId eid, void* actorKey);

// The host kerfur conversion: register a blueprint-internally spawned NPC (the turn-on output,
// spawned through a math call, so no spawn POST fires for it) as a host Npc Element without
// broadcasting a spawn; the sole wire signal for a conversion is KerfurConvert. Allocates,
// binds the actor and maps it. The host-range eid, or kInvalidId. Game thread.
coop::element::ElementId RegisterHostNpcSilent(void* actor, const std::wstring& className);

// The host kerfur conversion: release the dying NPC form's Element by eid without
// broadcasting a destroy (KerfurConvert carries the old eid). The destroy PRE minus the wire
// send. Game thread.
void ReleaseNpcElementSilent(coop::element::ElementId eid);

// The trust-boundary check both sides use: true iff `cls` derives from one of the allowlisted
// NPC bases, by a SuperStruct walk. False while the allowlist is not fully resolved.
bool IsAllowlistedClass(void* cls);

// Clear the per-session state: the tracked-NPC map, the counter, the bypass slot. The
// interceptor stays installed; the cached classes and the function pointer remain valid across
// a disconnect.
void OnDisconnect();

// Host only, the connect snapshot: re-send an entity spawn (class and current transform) for
// every already-spawned Npc Element to the freshly connected client, so a joiner materialises
// mirrors of NPCs that spawned before it joined. The client's existing receiver materialises
// each, and the mirror install is idempotent. Elements whose actor is not yet bound are
// skipped. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// The host-only pre-existing world-NPC walk lives in coop/creatures/npc_world_enum.h and
// consumes this header's host-side accessors.

// Host only, per pump tick: read each live Npc Element's transform and publish the batch for
// the net thread to fan out as unreliable poses, so the client mirrors move. A cheap no-op off
// the host or with no NPCs. Game thread.
void TickPoseStream();

// The resolved spawn references, valid after the install completes and the classes resolve,
// exposed so the dev spawn tool can drive a host NPC spawn through the same function the
// interceptor hooks, so the host alloc-and-broadcast path runs exactly as for a real spawn.
// False if not yet resolved.
struct DevSpawnRefs {
    void* beginDeferredFn = nullptr;  // GameplayStatics.BeginDeferredActorSpawnFromClass
    void* finishSpawnFn   = nullptr;  // GameplayStatics.FinishSpawningActor
    void* gsCdo           = nullptr;  // GameplayStatics CDO (the call self)
};
bool GetDevSpawnRefs(DevSpawnRefs& out);

}  // namespace coop::npc_sync
