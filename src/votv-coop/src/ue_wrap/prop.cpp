// ue_wrap/prop.cpp -- Aprop_C accessors (Stage 2 implementation).

#include "ue_wrap/prop.h"

#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace ue_wrap::prop {
namespace {

namespace P = profile;
namespace R = reflection;

// Cached prop_C UClass -- one-shot reflection lookup, re-validated via IsLive
// each call. A full level unload + reload could either (a) keep the same
// UClass live (typical for cooked-content classes), (b) destroy it and
// re-create with a different address, or (c) destroy it and reuse the address
// for another object. The IsLive check covers (b) and partially (c): when the
// slot's InternalIndex no longer matches the cached pointer, IsLive returns
// false and we re-resolve. NOT thread-safe to WRITE; readers run on the game
// thread (the only write site is PropBaseClass itself, called only on the
// game thread by the observer / autonomous-test paths).
void* g_propBaseCls = nullptr;

void* PropBaseClass() {
    if (g_propBaseCls && R::IsLive(g_propBaseCls)) return g_propBaseCls;
    g_propBaseCls = R::FindClass(P::name::PropClass);
    return g_propBaseCls;
}

// Read raw bytes at offset; small helpers so the field offsets are the only
// thing the code references (matches the existing engine.cpp pattern).
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
    // Walk SuperStruct chain. ~16 hops covers the deepest VOTV BP class
    // chain; UE4's own SCENE_COMPONENT_BASE inheritance is shallower. The
    // base prop_C itself counts (cls == base on the first compare).
    for (int hops = 0; hops < 16 && cls; ++hops) {
        if (cls == base) return true;
        cls = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(cls) + P::off::UStruct_SuperStruct);
    }
    return false;
}

// ---- IsKeyedInteractable -----------------------------------------------
//
// The non-Aprop_C "prop-shaped" interactable bases (RE 2026-05-27). We cache
// the 3 UClass pointers lazily; SuperStruct walk handles their subclass
// variants (_erie / _leaves / _wetConcrete).

namespace {
// Per-class sticky resolution. Each pointer is set ONCE on first successful
// FindClass and never re-walked. The std::atomic<void*> protects against
// parallel-anim worker threads that can call IsKeyedInteractable concurrently
// with the harness pump (audit Finding 1, 2026-05-27).
//
// Without per-class stickiness the previous code re-walked GUObjectArray
// 3x on every call until ALL three classes were live -- if even one class
// failed to load (e.g. cooked content not yet streamed in), the 125 Hz
// PropPose-emit hot path would burn ~89M wstring allocations per second,
// reproducing the bomb that took 19 GB RSS in the retired non_prop_entity
// pipeline.
std::atomic<void*> g_trashBitsPileCls{nullptr};
std::atomic<void*> g_garbageClumpCls{nullptr};
std::atomic<void*> g_actorChipPileCls{nullptr};
// Whole-set latch: once all 3 resolved we skip even the per-class atomic
// loads on the hot path. Set once, never cleared (UClasses are process-
// scoped; session boundary doesn't invalidate them).
std::atomic<bool> g_extrasAllResolved{false};

void ResolveExtraBases() {
    if (g_extrasAllResolved.load(std::memory_order_acquire)) return;
    void* trash = g_trashBitsPileCls.load(std::memory_order_acquire);
    void* clump = g_garbageClumpCls.load(std::memory_order_acquire);
    void* chip  = g_actorChipPileCls.load(std::memory_order_acquire);
    // Only walk GUObjectArray for classes we haven't yet found. Once a
    // pointer is in the atomic, we never re-walk -- even if R::IsLive
    // returns false later (no level reload covers these BP classes in
    // VOTV's current shape; on a hypothetical reload, the existing
    // ClassOf-based gate paths will simply miss until next session).
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

// Read-only accessors used by IsClassKeyedInteractable + GetInteractableKey.
inline void* TrashBitsPileCls() { return g_trashBitsPileCls.load(std::memory_order_acquire); }
inline void* GarbageClumpCls()  { return g_garbageClumpCls.load(std::memory_order_acquire); }
inline void* ActorChipPileCls() { return g_actorChipPileCls.load(std::memory_order_acquire); }

bool WalksToBase(void* cls, void* base) {
    if (!cls || !base) return false;
    for (int hops = 0; hops < 16 && cls; ++hops) {
        if (cls == base) return true;
        cls = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(cls) + P::off::UStruct_SuperStruct);
    }
    return false;
}
}  // namespace

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

bool IsTrashBitsPile(void* obj) {
    if (!obj) return false;
    ResolveExtraBases();
    return WalksToBase(R::ClassOf(obj), TrashBitsPileCls());
}

// AtrashBitsPile_C collect counters (v57 trash_pile_sync). Raw int32 fields per
// the CXX dump: amountA @0x0260, amountB @0x0264 (the displayed "uses" count is
// their SUM, formatted live by lookAt -- no refresh verb exists or is needed).
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

// (IsRngDivergentClass + ResolveRngDivergentBases + the mushroom class slot
// were RETIRED 2026-06-10, Fork B 2e: the adoption sweep's universe is now
// IsClassKeyedInteractable + the keyless chipPile lineage -- the same
// predicate the host snapshot expresses -- so the separate 4-class
// "RNG-divergent" notion is gone. RULE 2: deleted fully.)

// ---- GetInteractableKey ------------------------------------------------
//
// Aprop_C: direct field @0x02E0.
// AtrashBitsPile_C (Aactor_save_C lineage): direct field @0x0230.
// chipPile/clump (no native Key field per CXX dump): dispatch BP UFunction
//   GetKey(FName& out) via ProcessEvent. Cached per UClass.

namespace {
constexpr size_t kAactorSaveKeyOff = 0x0230;

// Per-class GetKey UFunction cache. game-thread mutated; observers that
// call us from a parallel-anim worker would race -- protect with atomic
// snapshot via the unordered_map's internal coherence isn't enough. Use
// a mutex; lookups are infrequent (per-spawn / per-destroy of these classes).
std::mutex g_getKeyFnMutex;
std::unordered_map<void*, void*> g_getKeyFnByClass;  // UClass* -> UFunction*

void* ResolveGetKeyFn(void* cls) {
    if (!cls) return nullptr;
    std::lock_guard<std::mutex> lk(g_getKeyFnMutex);
    auto it = g_getKeyFnByClass.find(cls);
    if (it != g_getKeyFnByClass.end()) return it->second;
    void* fn = R::FindFunction(cls, L"GetKey");
    g_getKeyFnByClass[cls] = fn;  // cache even null so we don't re-walk
    return fn;
}

R::FName CallGetKeyUFunction(void* obj) {
    void* cls = R::ClassOf(obj);
    void* fn = ResolveGetKeyFn(cls);
    if (!fn) return R::FName{0, 0};
    ue_wrap::ParamFrame f(fn);
    if (!ue_wrap::Call(obj, f)) return R::FName{0, 0};
    return f.Get<R::FName>(L"Key");
}
}  // namespace

R::FName GetInteractableKey(void* obj) {
    if (!obj) return R::FName{0, 0};
    if (IsDescendantOfProp(obj)) {
        return ReadField<R::FName>(obj, P::off::Aprop_Key);
    }
    // trashBitsPile (Aactor_save_C lineage) -- direct field
    ResolveExtraBases();
    void* cls = R::ClassOf(obj);
    if (WalksToBase(cls, TrashBitsPileCls())) {
        return ReadField<R::FName>(obj, kAactorSaveKeyOff);
    }
    // chipPile / clump -- BP UFunction dispatch
    if (WalksToBase(cls, GarbageClumpCls()) || WalksToBase(cls, ActorChipPileCls())) {
        return CallGetKeyUFunction(obj);
    }
    return R::FName{0, 0};
}

std::wstring GetInteractableKeyString(void* obj) {
    if (!obj) return {};
    const R::FName key = GetInteractableKey(obj);
    return R::ToString(key);
}

R::FName GetKey(void* prop) {
    if (!prop) return R::FName{0, 0};
    return ReadField<R::FName>(prop, P::off::Aprop_Key);
}

std::wstring GetKeyString(void* prop) {
    if (!prop) return {};
    const R::FName key = GetKey(prop);
    return R::ToString(key);
}

bool IsHeavy(void* prop) {
    if (!prop) return false;
    return ReadField<bool>(prop, P::off::Aprop_propData_heavy);
}

bool IsStatic(void* prop) {
    if (!prop) return false;
    return ReadField<bool>(prop, P::off::Aprop_Static);
}

bool IsFrozen(void* prop) {
    if (!prop) return false;
    return ReadField<bool>(prop, P::off::Aprop_frozen);
}

void WriteStatic(void* prop, bool on) {
    if (!prop) return;
    *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(prop) + P::off::Aprop_Static) = on;
}

void WriteFrozen(void* prop, bool on) {
    if (!prop) return;
    *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(prop) + P::off::Aprop_frozen) = on;
}

bool IsSleeping(void* prop) {
    if (!prop) return false;
    return ReadField<bool>(prop, P::off::Aprop_sleep);
}

std::wstring GetPropNameString(void* prop) {
    // Internal lineage gate (mirrors GetStaticMesh below): Name@0x0258 is a
    // stray byte on non-Aprop_C keyed interactables.
    if (!prop || !IsDescendantOfProp(prop)) return {};
    return R::ToString(ReadField<R::FName>(prop, P::off::Aprop_Name));
}

bool ReadRemoveWOrespawn(void* prop) {
    if (!prop) return false;
    return ReadField<bool>(prop, P::off::Aprop_removeWOrespawn);
}

bool WriteSpParityIdentity(void* prop, R::FName nameRow,
                           bool isStatic, bool removeWOrespawn,
                           bool frozen, bool sleep) {
    if (!prop || !IsDescendantOfProp(prop)) return false;
    auto* base = reinterpret_cast<uint8_t*>(prop);
    // NAME_None means "wire carried no row" (e.g. a pre-v54 peer would, and a
    // class whose CDO row is correct does) -- leave the CDO/default Name so
    // init() still resolves SOMETHING rather than an empty row.
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
    // Aprop_C ONLY. The fixed Aprop_StaticMesh offset (0x0238) is meaningless on
    // a non-Aprop_C keyed interactable (garbageClump/chipPile keep their mesh at
    // a DIFFERENT offset; 0x0238 is a stray byte there) -- reading it as a mesh
    // pointer and running physics on it is the reverted-2a use-after-free
    // ([[project-bug-trash-chippile-uaf-crash]]). Return null for non-Aprop_C so
    // EVERY caller treats them as physics-free / kinematic. We deliberately do
    // NOT resolve their real mesh per-class (that was 2a) -- these are transient
    // self-morphing actors; driving them via physics frees-then-derefs.
    if (!IsDescendantOfProp(prop)) return nullptr;
    return ReadField<void*>(prop, P::off::Aprop_StaticMesh);
}

// ---- chipType (trash variant) ------------------------------------------
// Resolved via reflection, NOT a fixed offset: chipType lives at 0x0238 on the
// chipPile/clump family but that SAME offset is StaticMesh on an Aprop_C, so a
// fixed-offset write would corrupt an Aprop_C mesh pointer. FindPropertyOffset
// returns -1 for any class lacking the property -> GetChipType returns 0 +
// SetChipType no-ops -> safe on ANY actor. Per-class offset + setTex cached
// (these run on the game thread from the spawn receiver / held-edge sender;
// the mutex mirrors g_getKeyFnMutex's parallel-anim-worker safety).
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
    // Repaint from the new variant via the game's own setTex. VERIFIED bytecode
    // (prop_garbageClump_C::setTex): SetMaterial(0, getChipPileType(chipType).GetMaterial(0))
    // on the clump's FIXED dirtball mesh -- a per-chipType MATERIAL swap, NOT a mesh swap.
    // (The proxy mirror replicates this directly in trash_proxy::SkinProxy.) No-op if the
    // class has no setTex (chipPile sets its mesh = getChipPileType(chipType) elsewhere).
    if (void* fn = ResolveSetTexFn(cls)) {
        ue_wrap::ParamFrame f(fn);
        ue_wrap::Call(actor, f);
    }
}

void* ResolvePileMesh(uint8_t chipType, void* worldContext) {
    // Ulib_getFunc_C::getChipPileType(Type, __WorldContext) -> UStaticMesh*: the
    // game's OWN chipType -> pile-mesh resolver. The client computes the trash
    // proxy's mesh EXACTLY as the game does (zero hardcoded asset paths; correct
    // for all 14 chipType variants). Dispatched on the lib's CDO (a
    // UBlueprintFunctionLibrary, so the call target is Default__lib_getFunc_C).
    // Caches the last non-null result as a fallback so a transient null (a
    // not-yet-streamed variant, or an out-of-range chipType) never leaves a proxy
    // invisible. Game-thread only (dispatches a UFunction; static locals are GT-serial).
    static void* sCdo = nullptr;
    static void* sFn = nullptr;
    static void* sLastGood = nullptr;
    if (!sFn || !sCdo) {
        if (!sFn) {
            if (void* cls = R::FindClass(L"lib_getFunc_C"))
                sFn = R::FindFunction(cls, L"getChipPileType");
        }
        if (!sCdo) sCdo = R::FindClassDefaultObject(L"lib_getFunc_C");
    }
    if (!sFn || !sCdo || !worldContext) return sLastGood;
    void* mesh = nullptr;
    {
        ue_wrap::ParamFrame f(sFn);
        f.Set<uint8_t>(L"Type", chipType);          // TEnumAsByte<enum_chipPileType::Type>
        f.Set<void*>(L"__WorldContext", worldContext);
        if (ue_wrap::Call(sCdo, f)) mesh = f.Get<void*>(L"ReturnValue");
    }
    if (mesh && R::IsLive(mesh)) { sLastGood = mesh; return mesh; }
    return sLastGood;  // transient null -> last good (never invisible)
}

std::wstring GetClassName(void* prop) {
    if (!prop) return {};
    return R::ClassNameOf(prop);
}

NearestResult FindNearest(const FVector& anchor, bool wantHeavy, ScanStats* outStats) {
    NearestResult best;
    ScanStats stats;
    void* base = PropBaseClass();
    if (!base) {
        if (outStats) *outStats = stats;
        return best;
    }
    float bestD2 = 1e18f;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        ++stats.totalScanned;
        // Fast filter FIRST: super-chain walk is a few pointer compares with
        // no allocation. Most of GUObjectArray (>99% of ~237k entries) isn't
        // an Aprop_C derivative and shouldn't pay for a wstring allocation.
        if (!IsDescendantOfProp(obj)) continue;
        // Now safe to allocate: skip CDOs (Default__<Class>) -- they have no
        // world location and aren't grabbable instances. With the filter
        // order above, this allocates only ~candidate count (~2k) wstrings
        // instead of ~237k.
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;
        ++stats.candidates;
        const bool heavy = IsHeavy(obj);
        if (heavy) ++stats.totalHeavy;
        if (wantHeavy && !heavy) continue;
        // Read world location via the BP-callable K2_GetActorLocation (the
        // generic AActor path; works for any subclass without per-prop
        // logic).
        const FVector loc = engine::GetActorLocation(obj);
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

// Cached UFunction resolutions for the host-side velocity capture. Resolved
// lazily on first GetPhysicsVelocity call; both UFunctions are on
// UPrimitiveComponent (engine-stable) so a single class lookup suffices. The
// frame is a fixed 32 B (BoneName FName 8 + FVector ReturnValue 12 + pad);
// kept inline with no allocation. Game-thread only.
struct PrimVelocityResolved {
    void*    cls       = nullptr;  // UPrimitiveComponent UClass
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
    if (g_pvr.ok && R::IsLive(g_pvr.cls)) return true;
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
    g_pvr.cls          = cls;
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
    // Linear -- 32-byte frame covers FName(8) + FVector(12) + padding.
    // Loud warn on frame overflow so a silent-zero-velocity regression after a
    // game update is diagnosable from the log (audit issue #5, 2026-05-24).
    unsigned char frameL[32] = {};
    if (g_pvr.linFrameSize > static_cast<int32_t>(sizeof(frameL))) {
        UE_LOGW("prop::GetPhysicsVelocity: linear frame size %d > 32 -- enlarge frameL buffer",
                g_pvr.linFrameSize);
        return out;
    }
    *reinterpret_cast<R::FName*>(frameL + g_pvr.linBoneOff) = R::FName{0, 0};
    if (!R::CallFunction(mesh, g_pvr.getLinFn, frameL)) return out;
    out.linearCmS = *reinterpret_cast<FVector*>(frameL + g_pvr.linRetOff);
    // Angular -- same shape, separate frame buffer.
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
                          const std::wstring& expectedPropName) {
    if (className.empty() || radiusCm <= 0.f) return nullptr;
    void* base = PropBaseClass();
    if (!base) return nullptr;
    const float r2 = radiusCm * radiusCm;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!IsDescendantOfProp(obj)) continue;
        // Filter order (audit IMPORTANT-2 2026-05-24 + audit #5 2026-05-25):
        // cheapest checks FIRST. IsLive is a pure flag read (FUObjectItem.
        // Flags @ +0x08) -- cheaper than ClassNameOf which allocates a
        // wstring per call. So: descendant -> IsLive -> CDO-name -> class-
        // name. (FindByKeyString has a different ordering for a different
        // reason -- it needs CDO/name first to skip stale dying-same-key
        // matches; here we just want the cheapest filter first.)
        if (!R::IsLive(obj)) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;
        // Class match (leaf name equality -- e.g. "Aprop_food_mushroom_C").
        if (R::ClassNameOf(obj) != className) continue;
        // v54 identity gate: same CLASS is not same PROP for generic Aprop_C
        // -- the list_props row `Name` is the identity (a 'cube' and a
        // 'cubicleP_1' wall panel are both class prop_C). When the wire
        // carries a row, require it to match, else two co-located DIFFERENT
        // props would fuzzy-merge and the rekey would bind the wrong object.
        // Empty expectedPropName = class-only match (pre-v54 sender / the
        // caller had no row).
        if (!expectedPropName.empty() && GetPropNameString(obj) != expectedPropName) continue;
        const FVector loc = engine::GetActorLocation(obj);
        const float dx = loc.X - anchor.X;
        const float dy = loc.Y - anchor.Y;
        const float dz = loc.Z - anchor.Z;
        if (dx * dx + dy * dy + dz * dz <= r2) return obj;
    }
    return nullptr;
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
        // IsLive FIRST -- it reads only the GUObjectArray slot flags; ClassOf below
        // dereferences obj's OWN memory, which a GC pass could have freed between the
        // null check and here (AV on a dying actor). Matches FindNearbySameClass's order.
        // (audit fix 2026-06-03, [[feedback-islive-unsafe-on-freed-cached-pointer]])
        if (!R::IsLive(obj)) continue;
        // chipPile family ONLY (NON-Aprop_C, so IsDescendantOfProp can't gate it).
        // WalksToBase is a few pointer compares; cheaper than the wstring CDO check.
        if (!WalksToBase(R::ClassOf(obj), chipBase)) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;  // skip CDOs
        const FVector loc = engine::GetActorLocation(obj);
        const float dx = loc.X - anchor.X;
        const float dy = loc.Y - anchor.Y;
        const float dz = loc.Z - anchor.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= bestD2) { bestD2 = d2; best = obj; }
    }
    if (best && outDist) *outDist = std::sqrt(bestD2);
    return best;
}

void* FindByKeyString(const std::wstring& keyString) {
    if (keyString.empty()) return nullptr;
    void* base = PropBaseClass();
    if (!base) return nullptr;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        // Fast filter: descendant check before the string compare (string
        // compare is more expensive than pointer chain walk).
        if (!IsDescendantOfProp(obj)) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;
        if (GetKeyString(obj) != keyString) continue;
        // Liveness gate: reject PendingKill / Unreachable matches. UE4 keeps
        // the dying actor in its GUObjectArray slot until GC purge -- without
        // this check we can return the OLD instance after a level reload
        // when the same Key is re-spawned on the fresh actor (same wire
        // string, fresh memory). Caller wants the LIVE one only.
        if (!R::IsLive(obj)) continue;
        return obj;
    }
    return nullptr;
}

namespace {

// Resolved once on first call: the UPrimitiveComponent UClass + the
// SetCollisionEnabled UFunction + its NewType param offset + frame size.
// SetCollisionEnabled is a native UFunction on UPrimitiveComponent (engine-
// stable across UE4.27); no BP override path to consider. Game-thread only.
struct SetCollisionEnabledResolved {
    void*   cls           = nullptr;
    void*   fn            = nullptr;
    int32_t frameSize     = 0;
    int32_t newTypeOff    = -1;
    bool    ok            = false;
};
SetCollisionEnabledResolved g_sce;

bool ResolveSetCollisionEnabled() {
    if (g_sce.ok && R::IsLive(g_sce.cls)) return true;
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
    g_sce.cls       = cls;
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
    // Frame: ECollisionEnabled::Type at `newTypeOff`. The enum is a uint8;
    // values:
    //   0 NoCollision, 1 QueryOnly, 2 PhysicsOnly, 3 QueryAndPhysics,
    //   4 ProbeOnly, 5 QueryAndProbe.
    // 3 == QueryAndPhysics, the default for movable physics props
    // (mushroom_C, container_C, etc.).
    constexpr uint8_t kQueryAndPhysics = 3;
    // 16 bytes is enough for a single uint8 param + padding. Loud-warn on
    // overflow so a future UE update enlarging the frame is diagnosable.
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
