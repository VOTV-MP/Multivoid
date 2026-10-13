// ue_wrap/actors/prop.cpp -- the prop accessors: class tests for the prop and pile lineages,
// the save-parity fields, the chip type, mesh and physics reads, and the GUObjectArray finders.
// See ue_wrap/actors/prop.h; the keys are ue_wrap/actors/prop_keys.cpp, the physics-state flags
// ue_wrap/actors/prop_flags.

#include "ue_wrap/actors/prop.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/object_index.h"  // the fuzzy dedupe: one class and its instances
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/desk/tape_caddy.h"  // the reel Progress reader and writer

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace ue_wrap::prop {
namespace {

namespace P = profile;
namespace R = reflection;

// The prop base UClass, resolved once and re-validated through the cached reference on each
// call: a level reload can keep the class, destroy and re-create it elsewhere, or reuse the
// address, and the liveness check covers the last two. Written only on the game thread, by
// PropBaseClass itself.
ue_wrap::CachedObjRef g_propBaseCls;

void* PropBaseClass() {
    if (g_propBaseCls.Alive()) return g_propBaseCls.Raw();
    g_propBaseCls.Set(R::FindClass(P::name::PropClass));
    return g_propBaseCls.Raw();
}

// Raw reads at an offset, so the field offsets are the only thing the code references.
template <typename T>
inline T ReadField(void* base, size_t off) {
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off);
}

}  // namespace

bool IsDescendantOfProp(void* obj) {
    if (!obj) return false;
    void* cls = R::ClassOf(obj);
    return IsClassDescendantOfProp(cls);
}

bool IsClassDescendantOfProp(void* cls) {
    if (!cls) return false;
    void* base = PropBaseClass();
    if (!base) return false;
    // The SuperStruct chain; 16 hops covers the deepest VOTV blueprint chain, and the base itself
    // counts.
    for (int hops = 0; hops < 16 && cls; ++hops) {
        if (cls == base) return true;
        cls = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(cls) + P::off::UStruct_SuperStruct);
    }
    return false;
}

// IsKeyedInteractable: the non-prop interactable bases (the trash pile, the garbage clump, the
// chip pile). The three class pointers are cached lazily, and the SuperStruct walk covers their
// subclass variants.

namespace {
// Per-class sticky resolution: each pointer is set once on the first successful FindClass and
// never re-walked. Atomics, since observers on parallel-anim worker threads call
// IsKeyedInteractable concurrently with the pump. Without stickiness a class that fails to load
// would make every call on the pose-emit hot path re-walk the GUObjectArray.
std::atomic<void*> g_trashBitsPileCls{nullptr};
std::atomic<void*> g_garbageClumpCls{nullptr};
std::atomic<void*> g_actorChipPileCls{nullptr};
// The whole-set latch: once all three are resolved the hot path skips even the per-class loads.
// Never cleared; UClasses are process-scoped.
std::atomic<bool> g_extrasAllResolved{false};

void ResolveExtraBases() {
    if (g_extrasAllResolved.load(std::memory_order_acquire)) return;
    void* trash = g_trashBitsPileCls.load(std::memory_order_acquire);
    void* clump = g_garbageClumpCls.load(std::memory_order_acquire);
    void* chip  = g_actorChipPileCls.load(std::memory_order_acquire);
    // Only the classes not yet found are walked for. A stored pointer is never re-walked even if
    // liveness later says otherwise; no level reload covers these classes.
    if (!trash) {
        trash = R::FindClass(L"trashBitsPile_C");
        if (trash) g_trashBitsPileCls.store(trash, std::memory_order_release);
    }
    if (!clump) {
        clump = R::FindClass(L"prop_garbageClump_C");
        if (clump) g_garbageClumpCls.store(clump, std::memory_order_release);
    }
    if (!chip) {
        chip = R::FindClass(L"actorChipPile_C");
        if (chip) g_actorChipPileCls.store(chip, std::memory_order_release);
    }
    if (trash && clump && chip) {
        g_extrasAllResolved.store(true, std::memory_order_release);
    }
}

inline void* TrashBitsPileCls() { return g_trashBitsPileCls.load(std::memory_order_acquire); }
inline void* GarbageClumpCls()  { return g_garbageClumpCls.load(std::memory_order_acquire); }
inline void* ActorChipPileCls() { return g_actorChipPileCls.load(std::memory_order_acquire); }

// The per-class-name trash kind: a spawn burst asks this once per prop and FindClass is a full
// object-array walk on a miss, so only the dozen distinct class names ever walk. Game-thread
// serial, so no mutex.
struct TrashClassKind { bool isTrash = false; bool isClump = false; };
std::unordered_map<std::wstring, TrashClassKind> g_trashClassKind;

TrashClassKind ResolveTrashClassKind(const std::wstring& className, void* (*resolve)(const wchar_t*)) {
    if (className.empty()) return {};
    if (auto it = g_trashClassKind.find(className); it != g_trashClassKind.end()) return it->second;
    void* cls = resolve ? resolve(className.c_str()) : R::FindClass(className.c_str());
    if (!cls) return {};  // class not loaded yet -- do NOT memoise (it could load later)
    // Only after the name resolves: an unknown name (an attacker's, say) returns above without paying
    // the base resolves, which are their own object-array lookups while a trash base is unloaded.
    ResolveExtraBases();
    TrashClassKind k;
    k.isClump = WalksToBase(cls, GarbageClumpCls());
    k.isTrash = k.isClump || WalksToBase(cls, ActorChipPileCls());
    g_trashClassKind[className] = k;
    return k;
}


}  // namespace

// Declared in prop.h so container_contents_sync can test container descent with the tree's one
// SuperStruct walk.
bool WalksToBase(void* cls, void* base) {
    if (!cls || !base) return false;
    for (int hops = 0; hops < 16 && cls; ++hops) {
        if (cls == base) return true;
        cls = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(cls) + P::off::UStruct_SuperStruct);
    }
    return false;
}

bool IsTrashClassName(const std::wstring& className, void* (*resolve)(const wchar_t*)) {
    return ResolveTrashClassKind(className, resolve).isTrash;
}
bool IsClumpClassName(const std::wstring& className, void* (*resolve)(const wchar_t*)) {
    return ResolveTrashClassKind(className, resolve).isClump;
}

void* ChipPileClass() {
    ResolveExtraBases();
    return ActorChipPileCls();
}

void* GarbageClumpClass() {
    ResolveExtraBases();
    return GarbageClumpCls();
}

void* TrashBitsPileClass() {
    ResolveExtraBases();
    return TrashBitsPileCls();
}

bool IsClassKeyedInteractable(void* cls) {
    if (!cls) return false;
    if (IsClassDescendantOfProp(cls)) return true;
    ResolveExtraBases();
    return WalksToBase(cls, TrashBitsPileCls())
        || WalksToBase(cls, GarbageClumpCls())
        || WalksToBase(cls, ActorChipPileCls());
}

bool IsKeyedInteractable(void* obj) {
    if (!obj) return false;
    return IsClassKeyedInteractable(R::ClassOf(obj));
}

bool IsChipPile(void* obj) {
    if (!obj) return false;
    ResolveExtraBases();
    return WalksToBase(R::ClassOf(obj), ActorChipPileCls());
}

bool IsGarbageClump(void* obj) {
    if (!obj) return false;
    ResolveExtraBases();
    return WalksToBase(R::ClassOf(obj), GarbageClumpCls());
}

bool IsTrashActor(void* obj) {
    return IsChipPile(obj) || IsGarbageClump(obj);
}

bool IsTrashBitsPile(void* obj) {
    if (!obj) return false;
    ResolveExtraBases();
    return WalksToBase(R::ClassOf(obj), TrashBitsPileCls());
}

bool EnsureTrashBitsPileResolved() {
    // One resolve attempt per call; cheap once latched.
    ResolveExtraBases();
    return TrashBitsPileCls() != nullptr;
}

bool EnsurePropBaseResolved() {
    // The reseed hub consumer's resolve.
    return PropBaseClass() != nullptr;
}

// The trash pile's collect counters, raw int32 fields per the header dump; the displayed uses
// count is their sum, formatted live by the look-at, so no refresh verb is needed.
bool ReadTrashPileAmounts(void* actor, int32_t& a, int32_t& b) {
    if (!actor || !IsTrashBitsPile(actor)) return false;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(actor);
    a = *reinterpret_cast<const int32_t*>(base + 0x0260);
    b = *reinterpret_cast<const int32_t*>(base + 0x0264);
    return true;
}

bool WriteTrashPileAmounts(void* actor, int32_t a, int32_t b) {
    if (!actor || !IsTrashBitsPile(actor)) return false;
    uint8_t* base = reinterpret_cast<uint8_t*>(actor);
    *reinterpret_cast<int32_t*>(base + 0x0260) = a;
    *reinterpret_cast<int32_t*>(base + 0x0264) = b;
    return true;
}

R::FName GetPropName(void* prop) {
    // The lineage gate, as in GetStaticMesh: the Name offset is a stray byte on a non-prop keyed
    // interactable.
    if (!prop || !IsDescendantOfProp(prop)) return R::FName{0, 0};
    return ReadField<R::FName>(prop, P::off::Aprop_Name);
}

std::wstring GetPropNameString(void* prop) {
    const R::FName n = GetPropName(prop);
    return n.ComparisonIndex == 0 ? std::wstring() : R::ToString(n);
}

bool WriteSpParityIdentity(void* prop, R::FName nameRow,
                           bool isStatic, bool removeWOrespawn,
                           bool frozen, bool sleep) {
    if (!prop || !IsDescendantOfProp(prop)) return false;
    auto* base = reinterpret_cast<uint8_t*>(prop);
    // NAME_None means the wire carried no row; the default Name stays, so init still resolves a
    // row.
    if (nameRow.ComparisonIndex != 0) {
        *reinterpret_cast<R::FName*>(base + P::off::Aprop_Name) = nameRow;
    }
    *reinterpret_cast<bool*>(base + P::off::Aprop_Static)          = isStatic;
    *reinterpret_cast<bool*>(base + P::off::Aprop_removeWOrespawn) = removeWOrespawn;
    *reinterpret_cast<bool*>(base + P::off::Aprop_frozen)          = frozen;
    *reinterpret_cast<bool*>(base + P::off::Aprop_sleep)           = sleep;
    return true;
}

void* GetStaticMesh(void* prop) {
    if (!prop) return nullptr;
    // Props only: the static-mesh offset is meaningless on a non-prop keyed interactable (a clump
    // or a chip pile keeps its mesh elsewhere, and the offset is a stray byte there), and running
    // physics on that read is a use-after-free. Null for those, so every caller treats them as
    // physics-free; their real mesh is deliberately not resolved per class, since they are
    // transient self-morphing actors.
    if (!IsDescendantOfProp(prop)) return nullptr;
    return ReadField<void*>(prop, P::off::Aprop_StaticMesh);
}

std::wstring GetShownMeshName(void* prop) {
    void* comp = GetStaticMesh(prop);
    if (!comp) return {};
    // UStaticMeshComponent::StaticMesh, by reflection, once: the engine class never reloads.
    static int32_t s_off = -2;
    if (s_off == -2) {
        void* cls = R::FindClass(L"StaticMeshComponent");
        s_off = cls ? R::FindPropertyOffset(cls, L"StaticMesh") : -1;
    }
    if (s_off < 0) return {};
    void* mesh = ReadField<void*>(comp, static_cast<size_t>(s_off));
    return mesh ? R::ToString(R::NameOf(mesh)) : std::wstring(L"<none>");
}

// The chip type, resolved through reflection rather than a fixed offset: it lives at the
// offset that is the static mesh on a prop, so a fixed-offset write would corrupt a prop's mesh
// pointer. FindPropertyOffset returns -1 for a class without the property, so GetChipType reads
// 0 and SetChipType is a no-op on any other actor. The per-class offset and setTex are cached
// under a mutex, as the GetKey cache is.
namespace {
std::mutex g_chipTypeMutex;
std::unordered_map<void*, int32_t> g_chipTypeOffByClass;  // UClass* -> offset (-1 = none)
std::unordered_map<void*, void*>   g_setTexFnByClass;      // UClass* -> setTex UFunction*

int32_t ResolveChipTypeOffset(void* cls) {
    if (!cls) return -1;
    std::lock_guard<std::mutex> lk(g_chipTypeMutex);
    auto it = g_chipTypeOffByClass.find(cls);
    if (it != g_chipTypeOffByClass.end()) return it->second;
    const int32_t off = R::FindPropertyOffset(cls, L"chipType");
    g_chipTypeOffByClass[cls] = off;  // cache even -1 so we don't re-walk
    return off;
}

void* ResolveSetTexFn(void* cls) {
    if (!cls) return nullptr;
    std::lock_guard<std::mutex> lk(g_chipTypeMutex);
    auto it = g_setTexFnByClass.find(cls);
    if (it != g_setTexFnByClass.end()) return it->second;
    void* fn = R::FindFunction(cls, L"setTex");  // garbageClump has it; chipPile may not
    g_setTexFnByClass[cls] = fn;
    return fn;
}

std::unordered_map<void*, void*> g_initFnByClass;  // UClass* -> pile init() UFunction (nullptr = none)
void* ResolveInitFn(void* cls) {
    if (!cls) return nullptr;
    std::lock_guard<std::mutex> lk(g_chipTypeMutex);
    auto it = g_initFnByClass.find(cls);
    if (it != g_initFnByClass.end()) return it->second;
    void* fn = R::FindFunction(cls, L"init");  // actorChipPile 'init' skins the mesh from chipType
    g_initFnByClass[cls] = fn;                 // cache even nullptr (a clump has no init())
    return fn;
}

}  // namespace

uint8_t GetChipType(void* actor) {
    if (!actor) return 0;
    const int32_t off = ResolveChipTypeOffset(R::ClassOf(actor));
    if (off < 0) return 0;
    return *reinterpret_cast<const uint8_t*>(reinterpret_cast<const uint8_t*>(actor) + off);
}

void SetChipType(void* actor, uint8_t chipType) {
    if (!actor) return;
    void* cls = R::ClassOf(actor);
    const int32_t off = ResolveChipTypeOffset(cls);
    if (off < 0) return;  // not a chip-type actor (Aprop_C etc.) -- 0x0238 is StaticMesh there
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(actor) + off) = chipType;
    // Repaint from the new variant through the game's own setTex, which (per the clump blueprint)
    // sets material 0 of the fixed dirtball mesh to the pile-type mesh's material: a material swap,
    // not a mesh swap. A no-op for a class without setTex.
    if (void* fn = ResolveSetTexFn(cls)) {
        ue_wrap::ParamFrame f(fn);
        ue_wrap::Call(actor, f);
    }
}

void SetChipTypeAndRebuild(void* actor, uint8_t chipType) {
    if (!actor) return;
    SetChipType(actor, chipType);  // write the enum byte (+ setTex for a clump; a no-op setTex for a pile)
    // The pile builds its mesh in init, which sets the static mesh from the pile-type resolver, not
    // in setTex, so a pile needs an explicit init to re-skin from the new chip type. A no-op for a
    // class without init, such as a clump.
    if (void* fn = ResolveInitFn(R::ClassOf(actor))) {
        ue_wrap::ParamFrame f(fn);
        ue_wrap::Call(actor, f);
    }
}


NearestResult FindNearest(const FVector& anchor, bool wantHeavy, ScanStats* outStats,
                          const wchar_t* lineage, const NearbyExclusion& excluded) {
    NearestResult best;
    ScanStats stats;
    void* base = PropBaseClass();
    if (!base) {
        if (outStats) *outStats = stats;
        return best;
    }
    // Resolved once for the whole walk. A named class that is not loaded leaves the filter with
    // nothing to accept, and the empty answer is the right one: no actor of it exists here.
    void* wantCls = lineage ? R::FindClass(lineage) : nullptr;
    if (lineage && !wantCls) {
        if (outStats) *outStats = stats;
        return best;
    }
    float bestD2 = 1e18f;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        ++stats.totalScanned;
        // The fast filter first: the super-chain walk is a few pointer compares with no allocation,
        // and most of the GUObjectArray is not a prop derivative.
        if (!IsDescendantOfProp(obj)) continue;
        // Only candidates pay for the name string; CDOs have no world location and are not
        // grabbable.
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;
        ++stats.candidates;
        if (excluded.Holds(obj)) continue;
        if (wantCls) {
            void* cls = R::ClassOf(obj);
            if (!cls || !R::IsDescendantOfAny(cls, &wantCls, 1)) continue;
        }
        const bool heavy = IsHeavy(obj);
        if (heavy) ++stats.totalHeavy;
        if (wantHeavy && !heavy) continue;
        // The location through the blueprint-callable getter, which works for any subclass; an
        // unreadable one is never the nearest.
        FVector loc{};
        if (!engine::TryGetActorLocation(obj, loc)) continue;
        const float dx = loc.X - anchor.X;
        const float dy = loc.Y - anchor.Y;
        const float dz = loc.Z - anchor.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) {
            bestD2 = d2;
            best.prop = obj;
            best.className = R::ClassNameOf(obj);
            best.heavy = heavy;
            best.isStatic = IsStatic(obj);
            best.isFrozen = IsFrozen(obj);
        }
    }
    if (best.prop) {
        best.mesh = GetStaticMesh(best.prop);
        best.keyString = GetKeyString(best.prop);
        best.dist = std::sqrt(bestD2);
    }
    if (outStats) *outStats = stats;
    return best;
}

namespace {

// The velocity capture's cached resolutions, on first call; both functions are on
// UPrimitiveComponent, so one class lookup suffices. The frame is a fixed 32 bytes (the bone
// name, the vector result, padding), inline. Game thread only.
struct PrimVelocityResolved {
    ue_wrap::CachedObjRef cls;     // UPrimitiveComponent
    void*    getLinFn  = nullptr;
    void*    getAngFn  = nullptr;
    int32_t  linFrameSize = 0;
    int32_t  linBoneOff   = -1;
    int32_t  linRetOff    = -1;
    int32_t  angFrameSize = 0;
    int32_t  angBoneOff   = -1;
    int32_t  angRetOff    = -1;
    bool     ok = false;
};
PrimVelocityResolved g_pvr;

bool ResolvePrimVelocity() {
    if (g_pvr.ok && g_pvr.cls.Alive()) return true;
    g_pvr = {};
    void* cls = R::FindClass(P::name::PrimitiveComponentClass);
    if (!cls) {
        UE_LOGW("prop::GetPhysicsVelocity: PrimitiveComponent class not found");
        return false;
    }
    void* fnLin = R::FindFunction(cls, P::name::GetPhysicsLinearVelocityFn);
    void* fnAng = R::FindFunction(cls, P::name::GetPhysicsAngularVelocityInDegreesFn);
    if (!fnLin || !fnAng) {
        UE_LOGW("prop::GetPhysicsVelocity: UFunction lookup failed");
        return false;
    }
    g_pvr.cls.Set(cls);
    g_pvr.getLinFn     = fnLin;
    g_pvr.getAngFn     = fnAng;
    g_pvr.linFrameSize = R::FunctionFrameSize(fnLin);
    g_pvr.linBoneOff   = R::FindParamOffset(fnLin, L"BoneName");
    g_pvr.linRetOff    = R::FindParamOffset(fnLin, L"ReturnValue");
    g_pvr.angFrameSize = R::FunctionFrameSize(fnAng);
    g_pvr.angBoneOff   = R::FindParamOffset(fnAng, L"BoneName");
    g_pvr.angRetOff    = R::FindParamOffset(fnAng, L"ReturnValue");
    if (g_pvr.linBoneOff < 0 || g_pvr.linRetOff < 0 ||
        g_pvr.angBoneOff < 0 || g_pvr.angRetOff < 0) {
        UE_LOGW("prop::GetPhysicsVelocity: param offsets failed");
        g_pvr = {};
        return false;
    }
    g_pvr.ok = true;
    return true;
}

}  // namespace

VelocityState GetPhysicsVelocity(void* prop) {
    VelocityState out;
    if (!prop) return out;
    void* mesh = GetStaticMesh(prop);
    if (!mesh) return out;
    if (!ResolvePrimVelocity()) return out;
    // Linear: the 32-byte frame covers the name, the vector and padding; a loud warning on
    // overflow, so a silent zero velocity after a game update is diagnosable.
    unsigned char frameL[32] = {};
    if (g_pvr.linFrameSize > static_cast<int32_t>(sizeof(frameL))) {
        UE_LOGW("prop::GetPhysicsVelocity: linear frame size %d > 32 -- enlarge frameL buffer",
                g_pvr.linFrameSize);
        return out;
    }
    *reinterpret_cast<R::FName*>(frameL + g_pvr.linBoneOff) = R::FName{0, 0};
    if (!R::CallFunction(mesh, g_pvr.getLinFn, frameL)) return out;
    out.linearCmS = *reinterpret_cast<FVector*>(frameL + g_pvr.linRetOff);
    // Angular: the same shape, a separate frame.
    unsigned char frameA[32] = {};
    if (g_pvr.angFrameSize > static_cast<int32_t>(sizeof(frameA))) {
        UE_LOGW("prop::GetPhysicsVelocity: angular frame size %d > 32 -- enlarge frameA buffer",
                g_pvr.angFrameSize);
        return out;
    }
    *reinterpret_cast<R::FName*>(frameA + g_pvr.angBoneOff) = R::FName{0, 0};
    if (!R::CallFunction(mesh, g_pvr.getAngFn, frameA)) return out;
    out.angularDegS = *reinterpret_cast<FVector*>(frameA + g_pvr.angRetOff);
    out.ok = true;
    return out;
}

void* FindNearbySameClass(const std::wstring& className,
                          const FVector& anchor,
                          float radiusCm,
                          const std::wstring& expectedPropName,
                          const NearbyExclusion& excluded,
                          NearbyTrace* outTrace) {
    if (className.empty() || radiusCm <= 0.f) return nullptr;
    // The one loaded class of that name, and a prop's, or nothing: the name comes off the wire, and only a
    // prop's class may be adopted.
    void* const cls = object_index::ClassByName(className.c_str());
    if (!cls || !IsClassDescendantOfProp(cls)) return nullptr;
    // Its instances, collected before any engine call (the hand-out must not create or destroy), then in
    // slot order: the first accepted is the one in the lowest slot, what the old walk of the array met
    // first. The buffer is reused; nothing called below re-enters this query. Game thread.
    static std::vector<std::pair<int32_t, void*>> sInst;
    sInst.clear();
    object_index::ForEachInstance(
        cls,
        [](void* ctx, void* obj, int32_t idx) {
            static_cast<std::vector<std::pair<int32_t, void*>>*>(ctx)->emplace_back(idx, obj);
        },
        &sInst);
    std::sort(sInst.begin(), sInst.end());
    const float r2 = radiusCm * radiusCm;
    for (const auto& [idx, obj] : sInst) {
        // Cheapest first: the slot's flags, the child-actor read, the CDO's name. No string is built per
        // instance (the name compares render into the engine's scratch); the location read below is a
        // reflected call, made only for an instance of the right row.
        if (!R::IsLiveByIndex(obj, idx) || (R::SlotFlags(idx) & R::slot_flags::NotYetReadable)) continue;
        // A child-actor component's child (a kerfur's eye camera) never has independent cross-peer
        // identity, so a wire spawn of a standalone same-class prop near a kerfur must not
        // fuzzy-steal its eye.
        if (engine::IsChildActor(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        if (outTrace) ++outTrace->classMatches;
        // Same class is not the same prop for a generic prop: the list-props row name is the
        // identity (a cube and a wall panel are both prop_C). When the wire carries a row it must
        // match, or two co-located different props would fuzzy-merge and the rekey would bind the
        // wrong object; an empty expected name is a class-only match.
        // The row field read straight: the class is a prop's, proven once above.
        if (!expectedPropName.empty() &&
            !R::NameEquals(ReadField<R::FName>(obj, P::off::Aprop_Name), expectedPropName.c_str())) {
            if (outTrace) ++outTrace->rowNameRejects;
            continue;
        }
        FVector loc{};
        if (!engine::TryGetActorLocation(obj, loc)) {
            if (outTrace) ++outTrace->unreadRejects;
            continue;
        }
        const float dx = loc.X - anchor.X;
        const float dy = loc.Y - anchor.Y;
        const float dz = loc.Z - anchor.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > r2) {
            if (outTrace) {
                const float d = std::sqrt(d2);
                if (outTrace->nearestOutsideCm < 0.f || d < outTrace->nearestOutsideCm)
                    outTrace->nearestOutsideCm = d;
            }
            continue;
        }
        // Last, because it is the caller's knowledge rather than the query's: an actor inside the
        // radius that the caller has named is not a candidate at all.
        if (excluded.Holds(obj)) {
            if (outTrace) ++outTrace->excludedRejects;
            continue;
        }
        if (!outTrace) return obj;  // untraced: the first accepted is the answer
        outTrace->candidates.push_back(NearbyCandidate{obj, idx, std::sqrt(d2)});
    }
    return (outTrace && !outTrace->candidates.empty()) ? outTrace->candidates[0].actor : nullptr;
}

void* FindNearestChipPile(const FVector& anchor, float radiusCm, float* outDist) {
    if (outDist) *outDist = -1.f;
    if (radiusCm <= 0.f) return nullptr;
    ResolveExtraBases();
    void* chipBase = ActorChipPileCls();
    if (!chipBase) return nullptr;  // chipPile class not loaded yet
    const float r2 = radiusCm * radiusCm;
    float bestD2 = r2;
    void* best = nullptr;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        // IsLive first: it reads only the array slot flags, while ClassOf dereferences the object's
        // own memory, which a GC pass could have freed since the null check.
        if (!R::IsLive(obj)) continue;
        // The chip-pile family only, which the prop descendant test cannot gate; WalksToBase is a
        // few pointer compares, cheaper than the CDO name check.
        if (!WalksToBase(R::ClassOf(obj), chipBase)) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;  // skip CDOs
        FVector loc{};
        if (!engine::TryGetActorLocation(obj, loc)) continue;   // unreadable: never the nearest
        const float dx = loc.X - anchor.X;
        const float dy = loc.Y - anchor.Y;
        const float dz = loc.Z - anchor.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= bestD2) { bestD2 = d2; best = obj; }
    }
    if (best && outDist) *outDist = std::sqrt(bestD2);
    return best;
}

namespace {

// Resolved once: the UPrimitiveComponent class, the SetCollisionEnabled function, its NewType
// offset and frame size. A native engine function, with no blueprint override to consider. Game
// thread only.
struct SetCollisionEnabledResolved {
    ue_wrap::CachedObjRef cls;  // UPrimitiveComponent
    void*   fn            = nullptr;
    int32_t frameSize     = 0;
    int32_t newTypeOff    = -1;
    bool    ok            = false;
};
SetCollisionEnabledResolved g_sce;

bool ResolveSetCollisionEnabled() {
    if (g_sce.ok && g_sce.cls.Alive()) return true;
    g_sce = {};
    void* cls = R::FindClass(P::name::PrimitiveComponentClass);
    if (!cls) {
        UE_LOGW("prop::ForceRestoreDefaultCollision: PrimitiveComponent class not found");
        return false;
    }
    void* fn = R::FindFunction(cls, P::name::SetCollisionEnabledFn);
    if (!fn) {
        UE_LOGW("prop::ForceRestoreDefaultCollision: SetCollisionEnabled UFunction not found");
        return false;
    }
    g_sce.cls.Set(cls);
    g_sce.fn        = fn;
    g_sce.frameSize = R::FunctionFrameSize(fn);
    g_sce.newTypeOff = R::FindParamOffset(fn, L"NewType");
    if (g_sce.newTypeOff < 0) {
        UE_LOGW("prop::ForceRestoreDefaultCollision: NewType param offset not found");
        g_sce = {};
        return false;
    }
    g_sce.ok = true;
    return true;
}

}  // namespace

bool ForceRestoreDefaultCollision(void* prop) {
    if (!prop) return false;
    void* mesh = GetStaticMesh(prop);
    if (!mesh) return false;
    if (!ResolveSetCollisionEnabled()) return false;
    // The frame: the collision-enabled enum at the NewType offset, a byte: 0 NoCollision, 1
    // QueryOnly, 2 PhysicsOnly, 3 QueryAndPhysics, 4 ProbeOnly, 5 QueryAndProbe. 3 is the default
    // for a movable physics prop.
    constexpr uint8_t kQueryAndPhysics = 3;
    // 16 bytes covers a single byte parameter plus padding; a loud warning on overflow.
    unsigned char frame[16] = {};
    if (g_sce.frameSize > static_cast<int32_t>(sizeof(frame))) {
        UE_LOGW("prop::ForceRestoreDefaultCollision: frame size %d > 16 -- enlarge buffer",
                g_sce.frameSize);
        return false;
    }
    frame[g_sce.newTypeOff] = kQueryAndPhysics;
    if (!R::CallFunction(mesh, g_sce.fn, frame)) {
        UE_LOGW("prop::ForceRestoreDefaultCollision: CallFunction failed on mesh %p", mesh);
        return false;
    }
    return true;
}

}  // namespace ue_wrap::prop
