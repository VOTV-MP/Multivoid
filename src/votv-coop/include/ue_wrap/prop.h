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

}  // namespace ue_wrap::prop
