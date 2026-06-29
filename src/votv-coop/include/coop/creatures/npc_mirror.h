// coop/npc_mirror.h -- Inc3 client-side NPC mirror materialization.
//
// Extracted from coop/npc_sync.cpp (M-1 2026-05-29) per the soft-cap rule
// (npc_sync.cpp had grown to 1040 LOC; the receiver path is a cleanly
// separable subsystem). Architecturally mirrors the relationship between
// prop_lifecycle (host PRE/POST + Install) and prop_element_tracker
// (per-actor maps): one TU owns engine-side resolution + host broadcasts,
// the other TU owns the client receiver state + wire dispatch.
//
// Responsibilities:
//   - OnEntitySpawn(payload): wire-driven client-side NPC materialization.
//     Validates the payload against the npc_sync allowlist (via
//     npc_sync::IsAllowlistedClass), resolves the actor class, drives
//     BeginDeferredActorSpawnFromClass + FinishSpawningActor (game-thread
//     UFunction calls), and installs the resulting Npc into
//     MirrorManager<Npc> with RegisterMirror'd ElementId == host's id.
//   - OnEntityDestroy(payload): wire-driven mirror teardown via
//     MirrorManager::Take + K2_DestroyActor on the bound AActor*.
//   - DrainClientMirrors(): full client-mirror sweep called from
//     npc_sync::OnDisconnect; K2_DestroyActor every live actor + drain
//     the manager (each unique_ptr<Npc> dtor calls
//     Registry::UnregisterMirror outside the manager mutex --
//     ABBA-safe).
//
// Dependencies on coop::npc_sync (single direction, no cycles):
//   - GetSession()          -- for role()/connected() checks
//   - IsAllowlistedClass()  -- trust-boundary allowlist gate
//   - MarkIncomingNpcSpawn  -- bypass slot before BeginDeferred
//   - ClearIncomingNpcSpawn -- defensive clear on error
//   - SetClientRefs (below) -- pushed FROM npc_sync once Install resolves

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
struct EntitySpawnPayload;
struct EntityDestroyPayload;
}  // namespace coop::net

namespace coop::npc_mirror {

// Resolved UFunction references the client-side receivers need. Populated
// by coop::npc_sync::Install() when all of the underlying engine
// primitives are available (BeginDeferredSpawn + FinishSpawningActor +
// GameplayStatics CDO + AActor::K2_DestroyActor + ReturnValue param
// offset). Members may be null/-1 if their individual resolution failed
// (e.g. FinishSpawningActor missing on a degraded build) -- the receivers
// validate each field at call time and drop the packet with a warning on
// any miss.
struct ClientRefs {
    void*   spawnFn             = nullptr;  // UGameplayStatics::BeginDeferredSpawnFromClass
    void*   finishSpawnFn       = nullptr;  // UGameplayStatics::FinishSpawningActor
    void*   gsCdo               = nullptr;  // UGameplayStatics CDO (Self for ProcessEvent)
    int32_t spawnReturnParamOff = -1;       // 'ReturnValue' param offset on spawnFn
    void*   k2DestroyFn         = nullptr;  // AActor::K2_DestroyActor
};

// Push the resolved UFunction refs into the client-side receiver module.
// Called once by coop::npc_sync::Install when the AActor / K2_DestroyActor
// resolution succeeds (the same call path that also registers POST +
// PRE observers on the host side -- they are all the "all primitives
// available" gate). Safe to call multiple times; later calls overwrite
// stale entries with the same successfully-resolved pointers.
//
// Thread: game thread only (Install runs there). Receivers also run on
// the game thread (event_feed dispatches them via game_thread::Post),
// so plain non-atomic storage is safe -- no cross-thread reader exists.
void SetClientRefs(const ClientRefs& refs);

// Inc3 receiver (client-side, host-broadcast NPC materialization). Called
// from the event_feed dispatcher (via game_thread::Post) when a host
// EntitySpawn reliable packet arrives. Looks up the actor class by
// payload.className, validates it against the NPC allowlist, builds the
// FTransform, calls MarkIncomingNpcSpawn to bypass the suppressor, and
// invokes BeginDeferredActorSpawnFromClass + FinishSpawningActor to
// materialize the mirror. Registers the resulting Npc Element with the
// Registry as a MIRROR bound to the host's ElementId (Registry::
// RegisterMirror) so subsequent EntityDestroy routes back via lookup.
//
// Host echo: defensive no-op when role() == Host (host doesn't receive
// its own broadcasts; this is paranoia).
//
// v75: routes by payload.savePersisted. A host-spawned transient (savePersisted=0, no local twin)
// is fresh-spawned here via SpawnFreshNpcMirror. A save-persisted NPC (savePersisted=1 -- the
// kerfur, which the joining client ALSO loaded from the transferred save) is handed to
// coop::npc_adoption::ArmAdoption for deferred class-match adoption of its local twin (the local
// twin spawns via un-hookable EX_CallMath; its int_save key is RANDOM per peer so key-equality is
// impossible -- only class + untracked-local is portable).
void OnEntitySpawn(const coop::net::EntitySpawnPayload& payload);

// Fresh-spawn a host NPC mirror: BeginDeferred + FinishSpawning a NEW actor of `actorClass`, park
// it (CMC + actor ticks off; kerfur AI-timer neutralize), and Install it as mirror `elementId`.
// `actorClass` must already be resolved + allowlist-validated by the caller. Used by OnEntitySpawn
// for host-spawned transients AND by coop::npc_adoption as the TIMEOUT FALLBACK when a save-
// persisted NPC's local twin never materialized. Returns true on success. Game thread; requires
// SetClientRefs to have run.
bool SpawnFreshNpcMirror(const std::wstring& classW, void* actorClass, uint32_t elementId,
                         float locX, float locY, float locZ,
                         float rotPitch, float rotYaw, float rotRoll);

// Bind an ALREADY-SPAWNED local actor as the host mirror for `elementId` (vs SpawnFreshNpcMirror
// which spawns a new one). Used to ADOPT the client's own conversion result on a kerfur turn-on:
// the client's local game spawned the kerfur NPC via the un-hookable EX_CallMath path, the poll
// parked it (kerfur_convert::ClaimConversionGhosts), and this binds THAT real actor as the mirror
// -- no fresh-spawn beside it (the "two kerfurs out of one object" dupe) and no destroy/respawn
// pop. Adopting the real game-spawned actor is camera-safe (it is fully initialized, same as the
// v75 save-twin adoption). Builds the Npc Element, Installs it as mirror `elementId`, parks it
// host-driven. Returns true on success; on a duplicate-eid / Registry collision returns false
// WITHOUT destroying the actor (the caller / ghost-cleanup owns it). Game thread; requires
// SetClientRefs to have run.
bool AdoptExistingNpcAsMirror(void* actor, uint32_t elementId, const std::wstring& classW);

// Targeted K2_DestroyActor on a single local NPC actor. Used by kerfur_convert's orphan cleanup
// to destroy a parked conversion ghost the host never confirmed (sentient-kerfur reject / dropped
// request) before it can be grabbed into a client-eid dupe. No-op if K2_DestroyActor is unresolved
// or the actor is not live. Game thread.
void DestroyLocalNpcActor(void* actor);

// Inc3 receiver (client-side, host-broadcast NPC teardown). Resolves
// the mirror Element via MirrorManager<Npc>::Take(payload.elementId),
// pulls the AActor* off it, calls K2_DestroyActor, then drops the
// unique_ptr<Npc> -- the dtor unregisters from the Registry.
void OnEntityDestroy(const coop::net::EntityDestroyPayload& payload);

// Full sweep: K2_DestroyActor every bound client-mirror actor + drain
// the MirrorManager (each Npc destructor calls Registry::UnregisterMirror
// outside the manager mutex -- ABBA-safe with Registry::m_mutex).
// Called from coop::npc_sync::OnDisconnect.
//
// Thread: game thread only (K2_DestroyActor is a UFunction call;
// ProcessEvent is GT-only). Caller is expected to be on the GT.
void DrainClientMirrors();

// v75 CLIENT world-swap teardown: drop every client mirror whose actor was freed by a level load
// (a dangling Element -- the host sends no EntityDestroy on a client world-swap). RELEASE-ONLY (no
// K2_DestroyActor: the actor is gone); still-live mirrors are KEPT. Without this, a save-transfer
// join's second level load cannot re-mirror/re-adopt the same host eid (OnEntitySpawn + ArmAdoption
// early-return on Get(eid)!=nullptr against the stale Element). Called from
// npc_adoption::OnClientWorldReady. Game thread only.
void PruneDeadClientMirrors();

// CLIENT-side host-authoritative reconciliation (2026-06-13). Destroy every
// live allowlisted-NPC actor on this client that is NOT a host-streamed mirror
// (i.e. not bound to an Npc in MirrorManager<Npc>). Such an actor is a "ghost":
// a save-load NPC that spawned before the suppressor armed (save-transfer join
// loads the host's save -- with its NPCs -- before npc_sync::Install completes),
// or any client BP spawn that escaped suppression. Returns the count destroyed.
//
// This enforces the invariant "on a client, the only allowlisted NPCs are host
// mirrors" (MTA shape: client elements are server-authoritative; a join/resync
// reconciles the client set to the server's). ORDER-INDEPENDENT of mirror
// arrival: a tracked mirror is in the snapshot and is KEPT whether it
// materialized before or after this sweep, so it is safe to run any time after
// npc_sync::IsInstalled() (allowlist resolved). No-op off the client.
//
// Thread: game thread only (K2_DestroyActor is a ProcessEvent call). Cold path:
// one GUObjectArray walk per call (caller fires it once per (re)announce), like
// npc_sync::RegisterExistingWorldNpcs.
int DestroyUntrackedClientNpcs();

// v37 CLIENT pose-stream drive: drain the latest EntityPose batch (Session::TakeRemoteNpcBatch)
// -> open an interp window per NPC (SetTargetNpcPose), then advance + drive EVERY live mirror
// (element::Npc::Tick) so the mirrors MOVE + animate. No-op on the host / when no mirrors. Called
// every net-pump tick on the game thread.
void TickClientNpcs();

}  // namespace coop::npc_mirror
