// coop/creatures/npc_mirror.h -- the client-side NPC mirror: materialisation, teardown and the
// pose drive. npc_sync owns the engine-side resolution and the host broadcasts; this module
// owns the client receiver state and the wire dispatch, and depends on npc_sync in one
// direction: the session, the allowlist gate, the incoming-spawn bypass and the refs pushed
// through SetClientRefs.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
struct EntitySpawnPayload;
struct EntityDestroyPayload;
}  // namespace coop::net

namespace coop::npc_mirror {

// The resolved UFunction references the receivers need, populated by the npc_sync install once
// the engine primitives resolve. A member may be null or -1 if its own resolution failed; the
// receivers validate each field at call time and drop the packet with a warning on a miss.
struct ClientRefs {
    void*   spawnFn             = nullptr;  // UGameplayStatics::BeginDeferredSpawnFromClass
    void*   finishSpawnFn       = nullptr;  // UGameplayStatics::FinishSpawningActor
    void*   gsCdo               = nullptr;  // UGameplayStatics CDO (Self for ProcessEvent)
    int32_t spawnReturnParamOff = -1;       // 'ReturnValue' param offset on spawnFn
    void*   k2DestroyFn         = nullptr;  // AActor::K2_DestroyActor
};

// Push the resolved refs into the receiver module; called by the npc_sync install when the
// resolution succeeds, and safe to repeat. Game thread only, like the receivers (the event feed
// posts them there), so plain storage suffices.
void SetClientRefs(const ClientRefs& refs);

// The client-side receiver for a host EntitySpawn. Looks up the actor class by name, validates
// it against the NPC allowlist, and routes by whether the client has a local twin: a
// host-spawned transient is fresh-spawned here; a save-persisted NPC (the kerfur, which the
// joining client also loaded from the transferred save) is handed to npc_adoption for a
// deferred class-match adoption of its local twin, whose save key is random per peer, so only
// class plus untracked-local is portable. A spawn tagged with a converting eid adopts the
// client's own parked conversion ghost; a retire tag arms the teardown of a stale off-prop
// mirror. A no-op on the host.
void OnEntitySpawn(const coop::net::EntitySpawnPayload& payload);

// Fresh-spawn a host NPC mirror: a deferred spawn and finish of a new actor of `actorClass`,
// parked (character ticks off, the kerfur AI timers neutralised) and installed as mirror
// `elementId`. The class must already be resolved and allowlist-validated. Also npc_adoption's
// timeout fallback when a save-persisted NPC's local twin never materialised. Game thread;
// requires SetClientRefs to have run.
bool SpawnFreshNpcMirror(const std::wstring& classW, void* actorClass, uint32_t elementId,
                         float locX, float locY, float locZ,
                         float rotPitch, float rotYaw, float rotRoll,
                         float scaleX = 1.f, float scaleY = 1.f, float scaleZ = 1.f);  // the spawn scale, sanitised by the caller; the adopt paths bind existing actors

// Bind an already-spawned local actor as the host mirror for `elementId`: the client's own
// conversion result on a kerfur turn-on, which its game spawned through the EX_CallMath path
// ProcessEvent cannot see and the poll parked. Binding that real actor avoids a second kerfur
// beside it and a destroy-respawn pop, and it is fully initialised, so it is camera-safe.
// Returns false on a duplicate-eid collision without destroying the actor, which the ghost
// cleanup owns. Game thread; requires SetClientRefs to have run.
bool AdoptExistingNpcAsMirror(void* actor, uint32_t elementId, const std::wstring& classW);

// A targeted destroy of one local NPC actor: the conversion ghost cleanup uses it on a ghost
// the host never confirmed, before it can be grabbed into a client-eid dupe. A no-op if the
// destroy UFunction is unresolved or the actor is not live. Game thread.
void DestroyLocalNpcActor(void* actor);

// The client-side receiver for a host EntityDestroy: takes the mirror out of the manager,
// destroys its actor and drops it; the destructor unregisters from the Registry.
void OnEntityDestroy(const coop::net::EntityDestroyPayload& payload);

// Full sweep: destroy every bound client-mirror actor and drain the manager; each destructor
// unregisters outside the manager mutex, so the lock order against the Registry is safe.
// Called from the npc_sync disconnect. Game thread only.
void DrainClientMirrors();

// Client world-swap teardown: drop every client mirror whose actor a level load freed (the host
// sends no EntityDestroy for a client world swap). Release only, no destroy; live mirrors are
// kept. Without it a save-transfer join's second level load could not re-mirror or re-adopt
// the same host eid, since the receivers early-return on a stale Element at that id. Game
// thread only.
void PruneDeadClientMirrors();

// Client-side reconciliation: destroy every live allowlisted NPC actor on this client that is
// not a host-streamed mirror. Such an actor is a ghost: a save-load NPC that spawned before the
// suppressor armed (a save-transfer join loads the host's save, with its NPCs, before the
// install completes), or a client blueprint spawn that escaped suppression. Returns the count.
// Order-independent of mirror arrival: a tracked mirror is kept whether it materialised before
// or after the sweep, so it may run any time after the allowlist resolves. A no-op off the
// client. Game thread; one object-array walk per call, fired once per announce.
int DestroyUntrackedClientNpcs();

// The client pose drive: drain the latest EntityPose batch, open an interpolation window per
// NPC, then advance and drive every live mirror so they move and animate. A no-op on the host
// or with no mirrors. Every gameplay tick on the game thread.
void TickClientNpcs();

}  // namespace coop::npc_mirror
