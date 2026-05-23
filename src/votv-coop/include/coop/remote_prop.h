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
// zero, AddImpulse for the throw. Clears the cached pointer so the next
// PropPose has to re-resolve (covers the case where the host releases
// the prop and then re-grabs the SAME prop or a different one).
void OnRelease(const coop::net::PropReleasePayload& payload);

// Force-release: called on disconnect / level unload to put any cached
// prop back into normal physics state. Safe to call when not holding.
void ForceRelease();

}  // namespace coop::remote_prop
