// ue_wrap/actors/prop.h -- Aprop_C accessors. VOTV's physics-grabbable props share the base class
// Aprop_C (about 540 derivatives), and the fields live at fixed offsets in the base: propData
// (with `heavy` inside it), Static, frozen, Key and StaticMesh. sdk_profile.h holds every one
// of those offsets. The cross-peer identifier is the Key FName's ToString'd value, the save
// UUID, never the ComparisonIndex. The readers are plain memory reads with no liveness
// checks: the caller owns liveness.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ue_wrap/engine/engine.h"      // FVector
#include "ue_wrap/core/reflection.h"  // FName
#include "ue_wrap/actors/prop_flags.h"  // the physics-state flags and their verbs
#include "ue_wrap/actors/prop_events.h"  // the base class's grab prelude and throw

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

// A chip pile or a garbage clump: the two forms one trash entity rests in. A pile is the common
// one; a clump at rest is a pile that could not turn back (its re-pile refuses a hit on a simulating
// body, a slope, a holder whose hand is busy), and the game saves and loads it as a clump. False
// for null.
bool IsTrashActor(void* obj);

// The same two tests taken on a CLASS NAME rather than an instance, for a wire payload that
// names its class before any actor exists: true for the chip-pile and garbage-clump families, a
// descendant test against the two bases rather than a string match, memoised per name so a spawn
// burst walks the object array once per distinct class. An unloaded class answers false and is
// not memoised. Game thread.
bool IsTrashClassName(const std::wstring& className);
bool IsClumpClassName(const std::wstring& className);

// The two trash base UClasses themselves, from the same cache the tests above use; null until
// the class is loaded. A caller that needs the class rather than the test -- a hook resolving a
// verb declared on the base, a spawn of the mirror form -- takes it here instead of spelling the
// asset name a fourth time. Game thread.
void* ChipPileClass();
void* GarbageClumpClass();
// The dispenser pile's class, from the same cache, for a consumer that resolves a verb declared
// on it rather than testing an instance. Null until the class is loaded. Game thread.
void* TrashBitsPileClass();

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

// The Aprop_C visual identity, the FName Name: the list_props row init() resolves
// the mesh, mass and collision from (the CDO's row is the cube; loadData restores it before
// re-running init). Empty for null or a non-Aprop_C. Crosses the wire so a mirror constructs as
// the real prop. Game thread (an FName pool read).
std::wstring GetPropNameString(void* prop);

// The same row, as the FName the field actually holds: no render, no allocation. The form to use
// in a walk over the object array, where the string form costs a render and a std::wstring per
// prop in the world. Zero ComparisonIndex for null, a non-prop, or an unset row.
reflection::FName GetPropName(void* prop);

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

// Actors the caller knows carry no independent identity, and so must never be the answer even
// when they are the right class in the right place. The scan makes its own structural exclusions
// -- a class default object, a child-actor component's child -- but a display actor another
// system owns and destroys on its own schedule is not visible from here: only the system that
// spawned it knows. Empty means the caller has nothing to exclude, never that it forgot to ask.
struct NearbyExclusion {
    void* const* actors = nullptr;
    size_t       count  = 0;

    bool Holds(void* a) const {
        for (size_t i = 0; i < count; ++i)
            if (actors[i] == a) return true;
        return false;
    }
};

// The nearest Aprop_C derivative to `anchor`, or the nearest heavy one, by a GUObjectArray walk;
// CDOs skipped, positions read through GetActorLocation. The cost is a ProcessEvent dispatch per
// candidate (about 2,000 in a populated scene) through our own detour, so never per frame or
// inside an observer; the autotest calls it once per grab routine.
//
// `lineage`, when given, keeps only the derivatives of that class -- the food family is a lineage
// and not a name prefix, so a caller that wants a food asks for prop_food_C and gets all 154 of
// them. A class that is not loaded answers nothing, which is the truthful answer: nothing of it
// exists in this world. `excluded` drops candidates the caller has already tried and rejected, so
// a second call walks past them instead of returning the same one.
NearestResult FindNearest(const FVector& anchor, bool wantHeavy = false,
                          ScanStats* outStats = nullptr,
                          const wchar_t* lineage = nullptr,
                          const NearbyExclusion& excluded = {});

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

// One actor the nearby-same-class scan accepted: right class, right list_props row, inside the
// radius. `objIndex` is its GUObjectArray slot, which is the order the scan walks in.
struct NearbyCandidate {
    void*   actor    = nullptr;
    int32_t objIndex = -1;
    float   distCm   = 0.f;
};

// Everything the scan saw, filled only for a caller that asks. `candidates[0]` is what the scan
// returns, so a trace records the choice without changing it.
struct NearbyTrace {
    std::vector<NearbyCandidate> candidates;        // accepted, in scan order
    int32_t classMatches       = 0;                 // live, non-CDO actors of the class, any distance
    int32_t rowNameRejects     = 0;                 // ... rejected because the list_props row differed
    int32_t unreadRejects      = 0;                 // ... skipped because its location could not be read
    int32_t excludedRejects    = 0;                 // ... otherwise acceptable, but named by the caller
    float   nearestOutsideCm   = -1.f;              // nearest row-matching actor outside the radius, -1 if none
};

// The fuzzy dedupe for divergent-key spawns: the per-peer natural spawners (mushrooms,
// underground garbage) place the same logical entity with a different Key at a slightly different
// position, so the exact-key resolve fails and a duplicate follows. The first same-class Aprop
// within `radiusCm` of `anchor` in GUObjectArray order, or null; class by leaf name.
// `expectedPropName`, when non-empty, must also match the list_props row (cube and cubicleP_1 are
// both class prop_C; merging them rekeys the wrong object). One array walk per call; for spawn
// events, not hot paths. Passing `outTrace` walks the whole array instead of stopping at the
// first hit, which costs the rest of the walk and returns the same actor.
void* FindNearbySameClass(const std::wstring& className,
                          const FVector& anchor,
                          float radiusCm,
                          const std::wstring& expectedPropName,
                          const NearbyExclusion& excluded,
                          NearbyTrace* outTrace = nullptr);

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

}  // namespace ue_wrap::prop
