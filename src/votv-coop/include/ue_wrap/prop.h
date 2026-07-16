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

// True if `obj`'s UClass is one of the "prop-shaped" interactable lineages:
// Aprop_C (the canonical prop family) OR one of the non-Aprop garbage/trash
// families that expose the SAME BP interaction protocol (GetKey,
// canBeUsedHold, playerTryToHold, etc.) -- AactorChipPile_C,
// Aprop_garbageClump_C, AtrashBitsPile_C, plus their _erie/_leaves/
// _wetConcrete subclasses. Use this for grab + Init POST + K2_DestroyActor
// gating where the coop layer cares about ANY keyed-interactable, not just
// the C++-level Aprop_C chain. Class pointers are cached at first call;
// SuperStruct walk per call is O(<=16 hops). Returns false for null inputs.
//
// 2026-05-27: introduced after the STAGE 2 non_prop_entity_sync side-pipeline
// was retired (RULE 2). Existing Aprop_C-only gates were the reason chipPile/
// clump never synced -- they slip through IsDescendantOfProp despite having
// identical BP interaction surfaces.
bool IsKeyedInteractable(void* obj);
bool IsClassKeyedInteractable(void* cls);

// True iff `obj`'s class is actorChipPile_C or a subclass -- the grabbable trash PILE lineage
// ONLY (NOT the garbageClump held-ball, NOT trashBitsPile). Used by the clump re-grab dupe
// fix: the InpActEvt_use PRE observer fires PropDestroy(eid) only when the local player is
// grabbing a tracked PILE (lookAtActor). Class cached via ResolveExtraBases. False for null.
bool IsChipPile(void* obj);

// True iff `obj`'s class is prop_garbageClump_C or a subclass (_erie/_leaves/_wetConcrete and
// prop_dirtball_C which derives from it) -- the carried "ball" a chipPile morphs INTO on grab.
// Pointer-chain lineage test (no wstring). Used by the host-authoritative trash channel (trash_channel /
// local_streams) to identify the held clump on the proven held-object channel. False for null / non-clump.
bool IsGarbageClump(void* obj);

// AtrashBitsPile_C lineage test + its collect counters (v57 trash_pile_sync).
// amountA@0x0260 / amountB@0x0264 raw int32 per the CXX dump; the displayed
// "uses" readout is their SUM, formatted live by lookAt on every look -- raw
// writes are fully consistent (no refresh verb exists on the class). The
// Read/Write pair returns false for non-trashBitsPile actors (no stray reads).
bool IsTrashBitsPile(void* obj);
bool ReadTrashPileAmounts(void* actor, int32_t& a, int32_t& b);
bool WriteTrashPileAmounts(void* actor, int32_t a, int32_t b);

// (IsRngDivergentClass + ResolveRngDivergentBases RETIRED 2026-06-10, Fork B
// 2e: the adoption sweep's universe is IsClassKeyedInteractable + the keyless
// chipPile lineage -- the same predicate the host snapshot expresses. RULE 2.)

// Per-class Key reader for keyed-interactable actors. Aprop_C lineage reads
// the FName field directly @+0x02E0. AtrashBitsPile_C reads @+0x0230 (the
// Aactor_save_C lineage offset). chipPile/clump have NO native Key field --
// they expose Key only via the BP `GetKey(FName&)` UFunction; this helper
// resolves + caches the UFunction per UClass and dispatches via ProcessEvent.
// Returns NAME_None on failure (uninitialized Key, unresolved UFunction).
// Game-thread only for the UFunction-dispatch branch.
reflection::FName GetInteractableKey(void* obj);
std::wstring GetInteractableKeyString(void* obj);

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

// Write Aprop_C.Static / Aprop_C.frozen on a LIVE prop -- the wall-attachable
// stick/unstick mirror (v68 prop_stick_sync). Plain field writes; the caller
// follows with a ProcessEvent init() dispatch so the BP recomputes
// SetSimulatePhysics/collision from the new flags (the SP unstick shape:
// comp_wallAttachable.unstick = frozen=false + static=false + init()).
// Null-safe no-ops. CALLER must have established `prop` is a live
// Aprop_C-derived actor (same contract as the readers). Game thread.
void WriteStatic(void* prop, bool on);
void WriteFrozen(void* prop, bool on);

// Reads Aprop_C.sleep at +0x02DD. True = the prop is physics-SLEEPING (settled,
// not actively simulating). SP parity: Aprop_C::init() =
// SetSimulatePhysics(NOT(static||frozen||sleep)) -> a save-loaded settled prop has
// sleep=true and is therefore NON-simulating. The snapshot uses this so a settled
// host prop mirrors as kinematic on the client (no teleport-wake / penetration).
// CALLER must have established `prop` is a live Aprop_C-derived actor (the offset
// is meaningless on a non-Aprop_C keyed interactable -- gate on IsDescendantOfProp).
bool IsSleeping(void* prop);

// v54: reads the Aprop_C VISUAL IDENTITY -- FName `Name`@0x0258, the row into
// the list_props DataTable that init() resolves the mesh/mass/collision from
// (CDO default row = 'cube'; SP's loadData restores it before re-running
// init()). Returns empty for null / non-Aprop_C lineage (internal gate -- the
// offset is a stray byte elsewhere). The string crosses the wire in
// PropSpawnPayload.propName so a mirror constructs as the real prop, not the
// white cube. Game thread only (FName pool read).
std::wstring GetPropNameString(void* prop);

// v54: reads Aprop_C.removeWOrespawn at +0x02D9 (despawn-without-respawn;
// part of the bool set SP's getData/loadData round-trips). Same caller
// contract as IsStatic/IsSleeping: live Aprop_C-derived actor only.
bool ReadRemoveWOrespawn(void* prop);

// v54 SP-parity pre-Finish identity write: raw-writes Name@0x0258 (skipped
// when nameRow is NAME_None, ComparisonIndex==0) + Static/removeWOrespawn/
// frozen/sleep bools on a DEFERRED-spawned (BeginDeferredActorSpawnFromClass)
// Aprop_C mirror, BEFORE FinishSpawningActor. Finish then runs the UCS ->
// init() pass which resolves list_props[Name] into the true mesh/mass/
// collision/SetSimulatePhysics(!(Static||frozen||sleep)) -- the exact field
// set + ordering SP's own loadObjects->loadData->init() restore achieves,
// minus the cube flash (SP writes AFTER Finish and re-runs init()). Returns
// false (no writes) for null / non-Aprop_C lineage. Game thread only.
bool WriteSpParityIdentity(void* prop, reflection::FName nameRow,
                           bool isStatic, bool removeWOrespawn,
                           bool frozen, bool sleep);

// Reads Aprop_C.StaticMesh at +0x0238. This is the UPrimitiveComponent* the
// PhysicsHandle / PhysicsConstraint binds to (the param to
// GrabComponentAtLocation / SetConstrainedComponents).
void* GetStaticMesh(void* prop);

// chipType variant selector for the trash chip-pile / garbage-clump family
// (`TEnumAsByte<enum_chipPileType::Type>`, 1 byte, 14 variants). It picks the
// visual mesh via Ulib_getFunc_C::getChipPileType. The field lives on
// AactorChipPile_C AND Aprop_garbageClump_C (+ subclasses) -- and at the SAME
// struct offset (0x0238) that means StaticMesh on an Aprop_C, so these resolve
// the offset via reflection (FindPropertyOffset "chipType") which returns -1 on
// any class without the property: GetChipType then returns 0 and SetChipType is
// a NO-OP, so they are SAFE to call on a non-chip actor (never corrupts an
// Aprop_C mesh pointer). Per-class offset + setTex UFunction cached.
// [[project-bug-trash-chippile-uaf-crash]]
uint8_t GetChipType(void* actor);
// Writes chipType + dispatches the class's setTex() (if any) to repaint the
// mesh from the new variant. No-op for a class without a chipType property.
// Game-thread only (dispatches a UFunction). Used by the wire receiver so a
// mirrored clump shows the SAME trash variant the owner grabbed.
void SetChipType(void* actor, uint8_t chipType);

// Set a chipPile's chipType and REBUILD its appearance the game's OWN way: writes chipType, then dispatches
// the pile's init() -- which does getChipPileType(chipType) -> SetStaticMesh on BOTH the pile's StaticMesh
// AND Collision components (RE: actorChipPile 'init' export 80). This is exactly what loadData /
// loadPrimitiveData do after writing chipType. A fresh SpawnActor runs init via UserConstructionScript with
// the DEFAULT chipType=0, so a RUNTIME pile mirror must call this to show the host's variant -- an external
// single SetStaticMesh can't (the pile has two mesh components, and init also owns the collision mesh).
// For a clump, SetChipType's setTex already repaints and there is no init() (the call no-ops). No-op on a
// non-chip actor. Game-thread only (dispatches UFunctions).
void SetChipTypeAndRebuild(void* actor, uint8_t chipType);

// Resolve the chipType -> pile static-mesh the GAME uses, via
// Ulib_getFunc_C::getChipPileType(chipType, worldContext) dispatched on the lib's
// CDO. Returns a UStaticMesh* (the client computes the trash proxy's mesh exactly
// as the game does -- no hardcoded paths). Caches the last non-null result as the
// never-invisible fallback. `worldContext` is any live UObject world context.
// Game-thread only (dispatches a UFunction). Returns null only before the lib is
// loaded with no prior success.
void* ResolvePileMesh(uint8_t chipType, void* worldContext);

// (v52: TurnChipPileToPile RETIRED -- RULE 1+2. It dispatched actorChipPile_C::turnToPile,
// which the disassembly proves is the pile->clump GRAB morph: it spawns a clump (Max:=2.0),
// throws it, and K2_DestroyActor's self. The old v29 "landed pile impact sound" call thus
// DESTROYED the freshly-spawned landed pile + spawned a stray self-converting clump = the
// persistent clump DUPE. A landed pile needs no morph; it just spawns and sits.)

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
// research/findings/physics-grab/votv-throw-release-pipeline-RE-2026-05-24.md, Bug B).
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
//
// v54: `expectedPropName` -- when non-empty, a candidate must ALSO match it
// against its list_props row Name@0x0258 (same class is not same prop for
// generic Aprop_C: 'cube' and 'cubicleP_1' are both class prop_C; merging
// them rekeys the wrong object). Empty = class-only (sender carried no row).
void* FindNearbySameClass(const std::wstring& className,
                          const FVector& anchor,
                          float radiusCm,
                          const std::wstring& expectedPropName);

// Find the NEAREST chipPile-family actor (AactorChipPile_C + _erie/_leaves/
// _wetConcrete subclasses) to `anchor` within `radiusCm`, or null. These are
// NON-Aprop_C so the Aprop-only finders can't see them; this walks the chipPile
// base lineage instead. Used by the wire receiver to CONSUME its own copy of a
// shared world pile when the OTHER peer grabbed theirs: trash piles are loaded
// independently on each peer (key=None, no cross-peer id), so POSITION is the
// only cross-peer identity (same precedent as FindNearbySameClass's mushroom
// dedup). `outDist` (optional) gets the match distance in cm (-1 if no match).
// One-shot GUObjectArray walk; call on grab events only, not per-frame.
// [[project-bug-trash-chippile-uaf-crash]]
void* FindNearestChipPile(const FVector& anchor, float radiusCm, float* outDist = nullptr);

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
// 2026-05-25 RE (the standalone fall-through finding was never filed; nearest:
// research/findings/world-systems/votv-mushroom-state-RE-2026-05-24.md).
//
// Returns true on successful SetCollisionEnabled dispatch, false if any
// reflection lookup fails (StaticMesh null, PrimitiveComponent class missing,
// SetCollisionEnabled UFunction missing). Idempotent: writing
// QueryAndPhysics over an already-QueryAndPhysics state is a no-op in the
// engine's component-render path beyond a dirty-state flag flip.
bool ForceRestoreDefaultCollision(void* prop);

// v114 (L7): the per-class SAVE-SCALAR birth channel (PropSpawnPayload.savedScalar /
// PropDropIntentPayload.savedScalar). A "save scalar" is per-prop state VOTV's own save carries in
// struct_save.mFloat[0] and loadData restores -- state a mirror must receive AT BIRTH or a peer
// interacting with the mirror reads a CDO default and re-broadcasts it as truth (the L7 3rd-peer
// wrong-progress insert). Currently: the Aprop_reel_C lineage (Progress @0x0364, via
// ue_wrap::tape_caddy). Read returns false for classes with no save scalar (the caller leaves
// kHasSavedScalar clear); Apply is the ONE mirror-birth write site (post-Finish safe -- measured:
// the reel's consumers are lookAt + loadData only). Design:
// research/findings/computers-devices/votv-tape-caddy-L7-impl-DESIGN-2026-07-17.md.
bool ReadSavedScalarForClass(void* actor, float& out);
bool ApplySavedScalarForClass(void* actor, float value);

}  // namespace ue_wrap::prop
