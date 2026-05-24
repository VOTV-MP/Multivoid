// ue_wrap/prop.h -- Aprop_C accessors (Stage 2 of [[project-physics-object-pickup]]).
//
// VOTV's physics-grabbable props share one base class `Aprop_C` (~540 derivatives
// in cooked content). All meaningful fields live at fixed offsets in the base:
//
//   propData         @ +0x0260  (Fstruct_prop, size 0x78 -- contains the
//                                 `heavy` data-driven flag at +0x6C inside)
//   Static           @ +0x02D8  (a Static prop can't be grabbed)
//   frozen           @ +0x02DA  (BP-controlled freeze state)
//   Key (FName)      @ +0x02E0  (cross-peer stable identifier; the string is
//                                 the save UUID, baked into .sav + cooked
//                                 content -- DO NOT trust ComparisonIndex
//                                 across peers, only the ToString'd value)
//   StaticMesh       @ +0x0238  (UStaticMeshComponent*)
//
// All offsets verified by:
//   1. SDK reflection dump (CXXHeaderDump/prop.hpp)
//   2. Autonomous grab test runtime probe (logs match the SDK values exactly)
//
// These accessors are zero-allocation, zero-syscall: pure memory reads on a
// pointer the caller has already proven to be a live Aprop_C. NO defensive
// IsLive checks -- the caller owns liveness (grab observers fire on
// already-alive UObjects; the autonomous test scan filters CDOs).

#pragma once

#include <cstdint>
#include <string>

#include "ue_wrap/engine.h"      // FVector
#include "ue_wrap/reflection.h"  // FName

namespace ue_wrap::prop {

// True if `obj`'s UClass derives from the `prop_C` base class (via
// UStruct::SuperStruct chain walk). Cheap pointer compares + at most ~16 hops.
// Returns false for null `obj` or null prop-base lookup. Safe to call in a
// hot loop scanning GUObjectArray.
bool IsDescendantOfProp(void* obj);

// Same SuperStruct-chain check as IsDescendantOfProp but takes the UClass*
// DIRECTLY (no R::ClassOf step). Used by harness.cpp's subclass-aware Init
// observer install to test whether a UFunction's Outer (its owning UClass)
// is in the prop_C lineage. NOT a metaclass check -- pass the UClass you'd
// pass to FindFunction. Returns false for null inputs.
bool IsClassDescendantOfProp(void* cls);

// Reads Aprop_C.Key (FName) at +0x02E0. Returns {0,0} for null prop.
// CALLER must have already established `prop` IS an Aprop_C-derived live actor.
reflection::FName GetKey(void* prop);

// Resolves Aprop_C.Key to a wide string via FName::ToString. Empty for null.
// This IS the cross-peer-stable prop ID -- the save UUID baked into cooked
// content. Two peers loading the same save get the same string (different
// ComparisonIndex per-process, but the string is what we send on the wire).
std::wstring GetKeyString(void* prop);

// Reads Aprop_C.propData.heavy (the data-driven lift-vs-drag flag, NOT a
// mass comparison). True = uses UPhysicsConstraintComponent (heavyGrab),
// False = uses UPhysicsHandleComponent (grabHandle).
bool IsHeavy(void* prop);

// Reads Aprop_C.Static at +0x02D8. Static props can't be grabbed (skip the
// scan candidate / drop the grab packet).
bool IsStatic(void* prop);

// Reads Aprop_C.frozen at +0x02DA. BP-controlled freeze state; a frozen
// prop won't move even if grabbed (e.g. quest-locked containers).
bool IsFrozen(void* prop);

// Reads Aprop_C.StaticMesh at +0x0238. This is the UPrimitiveComponent* the
// PhysicsHandle / PhysicsConstraint binds to (the param to
// GrabComponentAtLocation / SetConstrainedComponents).
void* GetStaticMesh(void* prop);

// Convenience: the prop's UClass name (e.g. "prop_container_suitcase_C")
// via reflection::ClassNameOf. Used for diagnostics only.
std::wstring GetClassName(void* prop);

// Scan result for FindNearest.
struct NearestResult {
    void* prop = nullptr;
    void* mesh = nullptr;
    float dist = 0.f;
    std::wstring className;
    std::wstring keyString;
    bool heavy = false;
    bool isStatic = false;
    bool isFrozen = false;
};

// Scan stats (for logging / diagnostics).
struct ScanStats {
    int totalScanned = 0;     // GUObjectArray entries walked
    int candidates = 0;       // Aprop_C-derived live actors seen
    int totalHeavy = 0;       // candidates with propData.heavy=1
};

// Walks GUObjectArray, finds nearest Aprop_C derivative to `anchor`. If
// `wantHeavy` is true, only considers props with propData.heavy=1 (the
// nearest HEAVY drag prop). Skips CDOs (Default__*). Reads location via
// engine::GetActorLocation (BP-callable, works for any AActor subclass).
//
// COST PROFILE -- read before promoting to a hot path:
//   * ~237k pointer-compares for the GUObjectArray walk + super-chain check
//     (cheap; cache-friendly)
//   * ~candidate count wstring allocations for the CDO-skip name read
//     (~2,059 Aprop_C derivatives in the КПП spawn area; one allocation each)
//   * ~candidate count ProcessEvent DISPATCHES for engine::GetActorLocation
//     (each goes through our own detour, which walks both observer tables --
//     a 32-atomic-load amplifier per candidate). This is the dominant cost
//     for a populated scene.
// At ~2k candidates: ~64k atomic loads + ~2k allocations + 2k full PE
// dispatches per call. One-shot is fine; do NOT call per-frame or inside an
// observer. Cache the result if you need it across ticks. The autonomous
// test calls this once per grab routine; the production wire path would call
// it once per GRAB packet receive (to resolve sent_key_string -> local
// Aprop_C* -- prefer FindByKeyString which has the same cost profile).
//
// Pass `outStats` for the diagnostic counts.
NearestResult FindNearest(const FVector& anchor, bool wantHeavy = false,
                          ScanStats* outStats = nullptr);

// Resolve a prop by its KEY STRING -- the wire-receiver entry point.
// Walks GUObjectArray once, returns the FIRST Aprop_C-derived live actor
// whose GetKeyString() exactly matches `keyString`. Used by Stage 4's
// receiver to look up the local prop instance for a GRAB packet whose
// sender embedded the Key string. Returns nullptr if no match.
//
// Caching: callers should hold the returned Aprop_C* for the grab
// duration (use IsLive() to invalidate on world unload). One-shot lookup
// at GRAB packet handling; subsequent POSE packets use the cached pointer.
void* FindByKeyString(const std::wstring& keyString);

// Body velocity at THIS instant, read via UPrimitiveComponent reflection
// UFunctions on the prop's StaticMesh component. Used by the host's
// release-edge to capture the body's INHERITED tracking velocity (the
// dominant "вжух" launch energy per
// research/findings/votv-throw-release-pipeline-RE-2026-05-24.md, Bug B).
// linear in cm/s, angular in deg/s. ok=false if the prop / mesh / UFunctions
// can't be resolved or if the read fails. Game-thread only. One-shot
// reflection lookup cached internally; subsequent calls are 2 ProcessEvent
// dispatches each.
struct VelocityState {
    FVector linearCmS{0.f, 0.f, 0.f};
    FVector angularDegS{0.f, 0.f, 0.f};
    bool ok = false;
};
VelocityState GetPhysicsVelocity(void* prop);

// Phase 5S0 Gap I-1 (2026-05-24): fuzzy de-dupe for divergent-key spawns
// from per-peer-divergent natural spawners (AmushroomMaster_C,
// AmushroomSpawner_C, AundergroundGarbageSpawner_C, etc.). When both
// peers' independent spawners place the same LOGICAL entity at a slightly
// different position with a DIFFERENT Key, the receiver's exact-Key
// FindByKeyString fails -- duplicate ensues. This helper looks for a
// same-class Aprop within `radiusCm` of `anchor`. Returns the FIRST match
// (the first encountered in GUObjectArray order), or nullptr if none.
//
// IsLive-guarded. Skips Default__ CDOs. Walks GUObjectArray once per call
// (~237k slots, but the descendant + name check filter the active set to
// the same ~2000 prop candidates as FindNearest). Class match is by name
// equality on the actor's UClass leaf name (e.g. "Aprop_food_mushroom_C").
//
// Use sparingly: O(GUObjectSize) per call. OnSpawn is rare (mushroom
// growth ~few/sec at peak) so cost is acceptable. Document any per-tick
// caller -- not designed for hot paths.
void* FindNearbySameClass(const std::wstring& className,
                          const FVector& anchor,
                          float radiusCm);

// Restore the prop's StaticMesh component to QueryAndPhysics collision (the
// default for movable physics props). Used by remote_prop::OnSpawn to undo
// the NoCollision state set by spawnedNaturally() in a natural-spawn
// pipeline on the local peer, when the wire convergence path picks up the
// existing actor (exact-Key match or Gap-I-1 fuzzy match) and reuses it as
// the host-authoritative representative. The fresh-spawn path runs Aprop's
// own Init in FinishSpawningActor which sets the default itself; the
// convergence paths don't, hence this explicit restore.
//
// Currently applied for `prop_food_mushroom_C` (the cap mushroom, whose
// AmushroomSpawner_C::Spawn calls spawnedNaturally() on every fresh spawn,
// disabling collision until the BP graph would later restore it — but our
// fuzzy-match rekey hijacks the actor before that completes).
// 2026-05-25 RE: research/findings/votv-mushroom-fall-through-RE-2026-05-25.md.
//
// Returns true on successful SetCollisionEnabled dispatch, false if any
// reflection lookup fails (StaticMesh null, PrimitiveComponent class missing,
// SetCollisionEnabled UFunction missing). Idempotent: writing
// QueryAndPhysics over an already-QueryAndPhysics state is a no-op in the
// engine's component-render path beyond a dirty-state flag flip.
bool ForceRestoreDefaultCollision(void* prop);

}  // namespace ue_wrap::prop
