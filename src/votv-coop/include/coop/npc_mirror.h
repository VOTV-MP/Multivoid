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
void OnEntitySpawn(const coop::net::EntitySpawnPayload& payload);

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

}  // namespace coop::npc_mirror
