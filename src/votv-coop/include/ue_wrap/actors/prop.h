// ue_wrap/prop.h -- Aprop_C accessors. VOTV's physics-grabbable props share the base class
// Aprop_C (about 540 derivatives), and the fields live at fixed offsets in the base: propData
// (with `heavy` inside it), Static, frozen, Key and StaticMesh. sdk_profile.h holds every one
// of those offsets. The cross-peer identifier is the Key FName's ToString'd value, the save
// UUID, never the ComparisonIndex. The readers are plain memory reads with no liveness
// checks: the caller owns liveness.

#pragma once

#include <cstdint>
#include <string>

#include "ue_wrap/engine/engine.h"      // FVector
#include "ue_wrap/core/reflection.h"  // FName

namespace ue_wrap::prop {

// True if `obj`'s class derives from prop_C (a SuperStruct walk, at most 16 hops); false for null.
// Safe in a hot loop over the GUObjectArray.
bool IsDescendantOfProp(void* obj);

// The same check on a UClass directly (a UFunction's Outer, say); not a metaclass check. False
// for null.
bool IsClassDescendantOfProp(void* cls);

// The descent test the two above are built on: does `cls` reach `base` within 16 hops. Both are
// UClass pointers; false for either null.
bool WalksToBase(void* cls, void* base);

// True if `obj`'s class is a prop-shaped interactable lineage: Aprop_C, or one of the trash
// families with the same BP interaction protocol (GetKey, canBeUsedHold, playerTryToHold):
// AactorChipPile_C, Aprop_garbageClump_C, AtrashBitsPile_C and their _erie, _leaves and
// _wetConcrete subclasses. The gate for grab, Init and destroy observers where the coop layer
// means any keyed interactable; an Aprop_C-only gate let chipPile and clump through unsynced.
// Class pointers cached at first call; false for null.
bool IsKeyedInteractable(void* obj);
bool IsClassKeyedInteractable(void* cls);

// True only for actorChipPile_C or a subclass, the grabbable pile (not the clump, not
// trashBitsPile). False for null.
bool IsChipPile(void* obj);

// True only for prop_garbageClump_C or a subclass (the variants and prop_dirtball_C), the carried
// ball a chipPile morphs into on grab. A pointer-chain test, no strings. False for null.
bool IsGarbageClump(void* obj);

// The AtrashBitsPile_C test and its collect counters, amountA and amountB
// (raw int32); the displayed count is their sum, formatted live by lookAt, so raw writes are
// consistent (no refresh verb exists). The pair returns false for any other actor.
bool IsTrashBitsPile(void* obj);
// A readiness probe for the scan hub: attempts the extra-base resolve once and says whether
// trashBitsPile_C is known, so the hub benches that consumer instead of letting IsTrashBitsPile
// re-run FindClass per memo-missed class per pass. FindClass keeps its own cache, so a resolved
// class costs a lookup rather than a walk -- but that cache never memoises a MISS, so a class
// that is not loaded walks the whole object array on every single call.
bool EnsureTrashBitsPileResolved();

// True once the Aprop base class resolves; IsKeyedInteractable is vacuously false before that,
// and the re-seed consumer sits the pass out.
bool EnsurePropBaseResolved();
bool ReadTrashPileAmounts(void* actor, int32_t& a, int32_t& b);
bool WriteTrashPileAmounts(void* actor, int32_t a, int32_t b);

// The per-class Key reader for keyed interactables: an Aprop_C reads its own Key FName, a
// trashBitsPile the Aactor_save_C one, and a chipPile or clump has no native
// field and answers only through the BP GetKey UFunction, resolved and cached per class and
// dispatched through ProcessEvent. NAME_None on failure. Game thread for the dispatch branch.
reflection::FName GetInteractableKey(void* obj);
std::wstring GetInteractableKeyString(void* obj);

// The Aactor_save_C::key field for any descendant of that class, and only for one. The
// three-lineage reader above returns {0,0} for everything else, which ToString renders "None",
// byte-identical to a keyless actor; Acremator_C and Adish_C derive from Aactor_save_C and carry a
// key that reader reported as None. A separate function rather than a fourth branch: the reader has
// 33 call sites in the prop lanes. Class-gated, since AtriggerBase_C derives from AActor and keeps
// its key at another offset. "" when `obj` is not an actor_save descendant; a field read, any
// thread.
std::wstring GetActorSaveKeyString(void* obj);

// Aprop_C.Key; {0,0} for null. The caller has established a live Aprop_C.
reflection::FName GetKey(void* prop);

// The key as a string, the cross-peer-stable prop id: the same save UUID on both peers, whatever
// the per-process ComparisonIndex. Empty for null.
std::wstring GetKeyString(void* prop);

// Aprop_C.propData.heavy, the data-driven lift-versus-drag flag (not a mass compare): true uses
// the physics constraint (heavyGrab), false the physics handle (grabHandle).
bool IsHeavy(void* prop);

// Aprop_C.Static; a static prop cannot be grabbed.
bool IsStatic(void* prop);

// Aprop_C.frozen, the BP-controlled freeze; a frozen prop does not move when grabbed.
bool IsFrozen(void* prop);

// Static and frozen written on a live prop, the wall-attachable stick and unstick mirror; the
// caller follows with an init() dispatch so the BP recomputes SetSimulatePhysics and collision
// from the flags (the single-player unstick shape). Null-safe; game thread.
void WriteStatic(void* prop, bool on);
void WriteFrozen(void* prop, bool on);

// Aprop_C.sleep: true means physics-sleeping. Aprop_C::init sets
// SimulatePhysics(!(static || frozen || sleep)), so a save-loaded settled prop is non-simulating,
// and the snapshot mirrors it kinematic on the client. Aprop_C lineage only; the offset is a
// stray byte elsewhere.
bool IsSleeping(void* prop);

// The Aprop_C visual identity, the FName Name: the list_props row init() resolves
// the mesh, mass and collision from (the CDO's row is the cube; loadData restores it before
// re-running init). Empty for null or a non-Aprop_C. Crosses the wire so a mirror constructs as
// the real prop. Game thread (an FName pool read).
std::wstring GetPropNameString(void* prop);

// Aprop_C.removeWOrespawn, one of the bools the save round-trips. Live Aprop_C only.
bool ReadRemoveWOrespawn(void* prop);

// The pre-Finish identity write on a deferred-spawned Aprop_C mirror: Name (skipped
// for NAME_None) and the Static, removeWOrespawn, frozen and sleep bools before
// FinishSpawningActor, whose init pass then resolves list_props[Name] into the true mesh, mass,
// collision and SimulatePhysics, the field set and order the game's own loadData restore
// achieves, without the cube flash. False for null or a non-Aprop_C. Game thread.
bool WriteSpParityIdentity(void* prop, reflection::FName nameRow,
                           bool isStatic, bool removeWOrespawn,
                           bool frozen, bool sleep);

// Aprop_C.StaticMesh, the UPrimitiveComponent the physics handle or constraint binds.
void* GetStaticMesh(void* prop);

// The chipType variant selector of the pile and clump family (a 1-byte enum, 14 variants), which
// picks the mesh through Ulib_getFunc_C::getChipPileType. It lives at the same offset (0x0238)
// that is StaticMesh on an Aprop_C, so the offset resolves by reflection: -1 on a class without
// the property, where GetChipType returns 0 and SetChipType is a no-op, so both are safe on any
// actor. Per-class offset and setTex cached.
uint8_t GetChipType(void* actor);
// Writes chipType and dispatches the class's setTex, if any, to repaint; a no-op without the
// property. Game thread.
void SetChipType(void* actor, uint8_t chipType);

// A chipPile's chipType with the appearance rebuilt the game's own way: the write, then the
// pile's init(), which sets both the StaticMesh and the Collision component meshes from
// getChipPileType, as loadData does. A fresh SpawnActor runs init with the default chipType 0,
// so a runtime pile mirror needs this; a single SetStaticMesh cannot do it (two mesh
// components). A clump's setTex already repaints and it has no init. No-op elsewhere. Game
// thread.
void SetChipTypeAndRebuild(void* actor, uint8_t chipType);

// The pile static mesh for a chipType, as the game computes it: Ulib_getFunc_C::getChipPileType
// on the lib's CDO, with `worldContext` any live UObject; the last non-null result is cached as
// the never-invisible fallback. Null only before the lib loads with no prior success. Game
// thread.
void* ResolvePileMesh(uint8_t chipType, void* worldContext);

// FindNearest's result.
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

// FindNearest's scan counts.
struct ScanStats {
    int totalScanned = 0;     // GUObjectArray entries walked
    int candidates = 0;       // Aprop_C-derived live actors seen
    int totalHeavy = 0;       // candidates with propData.heavy=1
};

// The nearest Aprop_C derivative to `anchor`, or the nearest heavy one, by a GUObjectArray walk;
// CDOs skipped, positions read through GetActorLocation. The cost is a ProcessEvent dispatch per
// candidate (about 2,000 in a populated scene) through our own detour, so never per frame or
// inside an observer; the autotest calls it once per grab routine.
NearestResult FindNearest(const FVector& anchor, bool wantHeavy = false,
                          ScanStats* outStats = nullptr);

// A prop by key string: one GUObjectArray walk, the first live Aprop_C whose key matches, or
// null. The cold fallback behind the key index (prop_key_index), not a per-packet path.
void* FindByKeyString(const std::wstring& keyString);

// The body's velocity at this instant, through UPrimitiveComponent UFunctions on the StaticMesh;
// the host's release edge captures the inherited tracking velocity, the launch energy of a
// flick. Linear in cm/s, angular in deg/s; ok is false when the prop, mesh or UFunctions do not
// resolve. Two dispatches per call after the one-shot resolve. Game thread.
struct VelocityState {
    FVector linearCmS{0.f, 0.f, 0.f};
    FVector angularDegS{0.f, 0.f, 0.f};
    bool ok = false;
};
VelocityState GetPhysicsVelocity(void* prop);

// The fuzzy dedupe for divergent-key spawns: the per-peer natural spawners (mushrooms,
// underground garbage) place the same logical entity with a different Key at a slightly different
// position, so the exact-key resolve fails and a duplicate follows. The first same-class Aprop
// within `radiusCm` of `anchor` in GUObjectArray order, or null; class by leaf name.
// `expectedPropName`, when non-empty, must also match the list_props row (cube and cubicleP_1 are
// both class prop_C; merging them rekeys the wrong object). One array walk per call; for spawn
// events, not hot paths.
void* FindNearbySameClass(const std::wstring& className,
                          const FVector& anchor,
                          float radiusCm,
                          const std::wstring& expectedPropName);

// The nearest chipPile-family actor within `radiusCm`, or null; these are not Aprop_C, so the
// Aprop finders cannot see them. Trash piles load independently on each peer with key None, so
// position is their only cross-peer identity. `outDist` gets the distance, -1 on no match. One
// array walk; for grab events only.
void* FindNearestChipPile(const FVector& anchor, float radiusCm, float* outDist = nullptr);

// Restores the StaticMesh to QueryAndPhysics collision, the default for a movable prop. A
// natural spawner's spawnedNaturally disables collision until the BP graph restores it, and the
// convergence path (an exact or fuzzy match reusing the local actor) hijacks the actor before
// that completes; the fresh-spawn path runs init in FinishSpawningActor and needs nothing.
// False if the mesh or the UFunction does not resolve. Idempotent.
bool ForceRestoreDefaultCollision(void* prop);

// The per-class save-scalar birth channel: state VOTV's own save carries in struct_save.mFloat[0]
// and loadData restores, which a mirror must receive at birth or a peer interacting with it
// reads a CDO default and re-broadcasts it as truth. Currently the Aprop_reel_C lineage (Progress,
// through ue_wrap::tape_caddy). Read is false for a class with no scalar; Apply is
// the one mirror-birth write site, safe after Finish (the reel's consumers are lookAt and
// loadData).
bool ReadSavedScalarForClass(void* actor, float& out);
bool ApplySavedScalarForClass(void* actor, float value);

}  // namespace ue_wrap::prop
