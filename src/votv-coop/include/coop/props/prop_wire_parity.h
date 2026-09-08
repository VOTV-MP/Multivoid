// coop/props/prop_wire_parity.h -- SP-parity physics and collision reconciliation for
// wire-expressed props.
//
// ONE concept: make a wire-expressed prop's physics and collision state match what the game's own
// Aprop_C::init() or natural-spawn pipeline would have produced on this peer. No network logic, no
// spawn logic -- pure per-actor reconciliation, shared by the converge, fuzzy and fresh paths of
// the PropSpawn receiver. Game thread only: every function dispatches engine calls or touches
// engine objects.

#pragma once

#include <cstdint>
#include <string>

namespace coop::prop_wire_parity {

// True for prop classes whose locally-spawned instance lands with collision disabled (NoCollision)
// through a natural-spawn pipeline that calls spawnedNaturally(), so a wire-converged copy needs an
// explicit SetCollisionEnabled(QueryAndPhysics) restore. AmushroomSpawner_C::Spawn is the case that
// found it, by a mushroom falling through the floor. This is a fix at the symptom; the root fix,
// suppressing the client-side natural spawner, has its retirement plan in the .cpp comment.
bool IsCollisionRestoreClass(const std::wstring& cls);

// If the class needs collision restore, ForceRestoreDefaultCollision + log with the path label
// (exact-key / fuzzy / fresh-spawn) so a fall-through regression is diagnosable. No-op otherwise.
void RestoreCollisionIfNeeded(const wchar_t* pathLabel, const std::wstring& classW, void* actor);

// SP-parity kinematic reconcile: when the host prop is NOT simulating, force the client's
// pre-existing Aprop_C copy kinematic BEFORE any teleport-converge, because a kinematic body is not
// ejected when the teleport lands it in an RNG-divergent layout -- the ~320-prop penetration storm.
// Aprop_C only: GetStaticMesh is null for the chipPile and clump lineages, and driving their
// physics frees then dereferences. A no-op when the host prop IS simulating.
void ReconcileToHostPhysics(void* actor, uint8_t physFlags);

// SP-parity simulate state from the wire identity flags. Aprop_C::init() computes
// SetSimulatePhysics(NOT(static || frozen || sleep)), so a settled prop is simulate-ENABLED but
// asleep, which is what the game's PhysicsHandle grab requires -- force-kinematic mirrors were
// ungrabbable.
bool SpParitySimulate(uint8_t physFlags);

// Restore SP-parity physics AFTER a teleport-converge. Safe because the converge target is the
// host's rest pose in an identical transferred world, so the body wakes at a valid pose. Aprop_C
// only, for the same reason as ReconcileToHostPhysics.
void RestoreSpParityPhysicsAfterConverge(void* actor, uint8_t physFlags);

}  // namespace coop::prop_wire_parity
