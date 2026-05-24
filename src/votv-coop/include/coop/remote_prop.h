// coop/remote_prop.h -- receiver-side held-prop driver (v4 wire, Stage 4).
//
// Mirrors RemotePlayer in shape but for physics-prop grab replication:
//   * On the FIRST PropPose for a new Key, look up the local Aprop_C* by
//     string match over GUObjectArray (prop_wrap::FindByKeyString), call
//     SetSimulatePhysics(false) on its StaticMesh so PhysX stops simulating
//     it, and cache the pointer for the grab duration.
//   * Per subsequent PropPose: SetActorLocation + SetActorRotation on the
//     cached actor.
//   * On PropRelease (or a >500 ms PropPose gap, treated as implicit
//     release): SetSimulatePhysics(true) + AddImpulse if the impulse vector
//     is non-zero (throw), clear the cache.
//
// Methodology-wise this matches MTA's ucSyncTimeContext-style ownership
// transfer: the sender (host) is authoritative for the held prop; the
// receiver kinematically displays it. When the host releases, the prop's
// physics resumes on every peer independently (same world state because
// Aprop_C.heavy / .Static etc. are content-driven, identical cross-peer).

#pragma once

#include "coop/net/protocol.h"

namespace coop::net { class Session; }

namespace coop::remote_prop {

// Called every game-thread tick from the net pump. Drains the latest
// PropPose snapshot from `session` and applies it to the local prop
// instance (lookup-by-Key on first arrival, transform writes thereafter).
// Also handles the timeout-implicit-release if PropPose stream stops.
//
// Idempotent; runs only when the session is Connected.
void Tick(coop::net::Session& session);

// Handle an incoming RELIABLE PropRelease message. Receiver re-enables
// SimulatePhysics on the cached prop and, if the impulse vector is non-
// zero, AddImpulse for the throw + calls `Aprop_C.thrown(localPlayer)`
// to fire the natural throw-sound + particle-trail dispatch the prop's
// BP wires (RULE 1 -- same path NPCs and the local player use). Clears
// the cached pointer so the next PropPose has to re-resolve.
//
// `localPlayer` is passed so `prop.thrown` has a non-null Player param
// (the BP may null-check). Caller supplies the LOCAL mainPlayer_C ptr;
// the BP's "throw stats" attribution will credit local (semantically a
// minor inaccuracy in exchange for natural sound/effects).
void OnRelease(const coop::net::PropReleasePayload& payload, void* localPlayer);

// v5: handle an incoming PropSpawn (peer dropped an inventory item into
// the world). Resolves the class by leaf name, does a deferred SpawnActor
// at the wire transform, dispatches Aprop_C.setKey(receivedKey) BEFORE
// FinishSpawningActor (so Aprop_C.Init() doesn't overwrite Key with
// NewGuid), then FinishSpawningActor, then SetSimulatePhysics + optional
// initial velocity. Result: a matching local Aprop_X_C instance with
// the same Key. Subsequent PropPose updates resolve via the existing
// prop_wrap::FindByKeyString path.
//
// NO echo loop: receiver spawns directly through engine::SpawnActor (the
// deferred-spawn pair), NOT through UpropInventory_C.takeObj, so the
// takeObj POST observer (which is the sender hook) never fires.
void OnSpawn(const coop::net::PropSpawnPayload& payload);

// Force-release: called on disconnect / level unload to put any cached
// prop back into normal physics state. Safe to call when not holding.
void ForceRelease();

// Returns the local AActor* currently being kinematically driven by the
// PropPose stream (the peer's grab), or nullptr if no drive is active.
// Used by the Phase-5S0 snapshot de-dupe path to skip the transform
// convergence when a snapshot entry collides with a prop the peer is
// CURRENTLY holding -- otherwise the convergence stomps the active drive
// for one frame producing a visible teleport-pop (audit I-1 2026-05-24).
void* GetDriveActor();

// v5 Inc2 echo suppression: when this peer receives a PropSpawn and spawns
// the actor locally via OnSpawn, FinishSpawningActor triggers Aprop_C::Init
// which our Init POST observer hooks. Without suppression, that observer
// would re-broadcast a fresh PropSpawn back to the sender -- echo loop.
//
// OnSpawn calls MarkIncomingSpawn(actor) before FinishSpawningActor. The
// Init POST observer calls ConsumeIncomingSpawn(actor); if it returns true,
// the spawn came from a wire packet (skip broadcast). One-shot consumption:
// removed on read so subsequent legitimate Init events (which never happen
// on the same actor, but defensively) aren't masked.
//
// Same pattern for destroys: OnDestroy calls MarkIncomingDestroy(actor)
// before K2_DestroyActor; the K2_DestroyActor PRE observer calls
// ConsumeIncomingDestroy.
void MarkIncomingSpawn(void* actor);
bool ConsumeIncomingSpawn(void* actor);
void MarkIncomingDestroy(void* actor);
bool ConsumeIncomingDestroy(void* actor);

// v5 Inc2: handle an incoming PropDestroy. Resolves the key to a local
// AActor*, marks it as incoming-destroy (so the K2_DestroyActor observer
// doesn't echo), and calls K2_DestroyActor on it. Returns silently if no
// local actor with that key exists (the prop never replicated to us, or
// was already destroyed locally).
//
// 2026-05-25: `localPlayer` added for cross-peer destroy of a held prop.
// When client eats food that host is holding, host receives PropDestroy
// and we must call PHC.ReleaseComponent on host's mainPlayer.grabHandle
// BEFORE K2_DestroyActor -- otherwise UPhysicsHandleComponent::Tick
// Component reads GrabbedComponent (@+0xB0) as a dangling pointer the
// next frame and PhysX dereferences a freed body instance. The release
// path is gated on `mainPlayer.grabbing_actor == actor` so it no-ops
// for the common case where the destroyed prop isn't grabbed.
void OnDestroy(const coop::net::PropDestroyPayload& payload, void* localPlayer);

}  // namespace coop::remote_prop
