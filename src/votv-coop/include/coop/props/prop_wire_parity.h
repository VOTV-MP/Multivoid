// coop/props/prop_wire_parity.h -- SP-parity physics/collision reconciliation for wire-expressed
// props. Extracted from remote_prop_spawn.cpp 2026-07-12 (modular file-size rule: the file carried
// the OnSpawn receiver + the fresh-spawn materializer + these shared parity helpers; the helpers
// are consumed by BOTH survivors, so they get the one shared home instead of a copy in each).
//
// ONE concept: make a wire-expressed prop's physics/collision state match what SP's own
// Aprop_C::init() / natural-spawn pipeline would have produced on this peer. No network logic,
// no spawn logic -- pure per-actor state reconciliation, called from the converge/fuzzy/fresh
// paths of the PropSpawn receiver.
//
// Game thread only (every function dispatches engine calls / touches engine objects).

#pragma once

#include <cstdint>
#include <string>

namespace coop::prop_wire_parity {

// True for prop classes whose locally-spawned instance lands with collision disabled (NoCollision)
// via a natural-spawn pipeline that calls spawnedNaturally(), and whose wire-converged copy
// therefore needs an explicit SetCollisionEnabled(QueryAndPhysics) restore (mushroom fall-through
// RE 2026-05-25: AmushroomSpawner_C::Spawn -> spawnedNaturally() -> NoCollision). Interim fix at
// the symptom -- the RULE 1 retirement plan (suppress the client-side natural spawner instead)
// lives in the .cpp comment.
bool IsCollisionRestoreClass(const std::wstring& cls);

// If the class needs collision restore, ForceRestoreDefaultCollision + log with the path label
// (exact-key / fuzzy / fresh-spawn) so a fall-through regression is diagnosable. No-op otherwise.
void RestoreCollisionIfNeeded(const wchar_t* pathLabel, const std::wstring& classW, void* actor);

// SP-parity kinematic reconcile (2026-06-09 perf root fix): when the host prop is NOT simulating,
// force the client's pre-existing Aprop_C copy kinematic BEFORE any teleport-converge -- a
// kinematic body is not ejected when the teleport lands it in an RNG-divergent layout (the
// ~320-prop penetration storm). Aprop_C only (GetStaticMesh null for chipPile/clump lineages --
// driving their physics frees-then-derefs). No-op when the host prop IS simulating.
void ReconcileToHostPhysics(void* actor, uint8_t physFlags);

// SP-parity simulate state from the wire identity flags: SP's Aprop_C::init() computes
// SetSimulatePhysics(NOT(static || frozen || sleep)) -- a settled prop is simulate-ENABLED but
// asleep (what VOTV's PhysicsHandle grab requires; force-kinematic mirrors were ungrabbable,
// 2026-06-10).
bool SpParitySimulate(uint8_t physFlags);

// Restore SP-parity physics AFTER a teleport-converge (safe post-v56: the converge target is the
// host's rest pose in an IDENTICAL transferred world -- the body wakes at a valid pose). Aprop_C
// only (same scoping rationale as ReconcileToHostPhysics).
void RestoreSpParityPhysicsAfterConverge(void* actor, uint8_t physFlags);

}  // namespace coop::prop_wire_parity
