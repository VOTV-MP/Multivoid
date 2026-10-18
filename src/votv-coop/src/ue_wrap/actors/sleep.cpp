// ue_wrap/actors/sleep.cpp -- see ue_wrap/actors/sleep.h.

#include "ue_wrap/actors/sleep.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"  // GetWorldContext, TryGetActorLocation
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/core/sdk_profile.h"

#include <chrono>
#include <vector>

namespace ue_wrap::sleep {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

void* g_gmCls = nullptr;

int32_t g_offIsSleep = -1;     // mainGamemode.isSleep
int32_t g_offDreamProb = -1;   // mainGamemode.dreamProbability (-1 = the SP sentinel)
int32_t g_offBed = -1;         // mainGamemode.bed (Abed_C*)
int32_t g_offOrigPawn = -1;    // mainGamemode.origPawn (APawn*)
void* g_sleepFn = nullptr;     // gamemode.sleep(bed, dropItem, ignoreRagdoll)

void* g_gsCdo = nullptr;            // GameplayStatics CDO
void* g_setDilationFn = nullptr;    // SetGlobalTimeDilation(WorldContextObject, TimeDilation)
void* g_getDilationFn = nullptr;    // GetGlobalTimeDilation(WorldContextObject) -> float

std::chrono::steady_clock::time_point g_nextResolve{};
bool g_coreResolved = false;  // isSleep, dreamProbability, bed and the dilation setter: latched once they are
bool g_allResolved = false;   // the optional members as well: nothing is looked up after this

bool IsCdo(void* obj) { return R::NameStartsWith(R::NameOf(obj), L"Default__"); }

void OnStaticsInstance(void* ctx, void* obj, int32_t) {
    void*& found = *static_cast<void**>(ctx);
    if (!found && IsCdo(obj)) found = obj;
}

// The GameplayStatics default object: the one listed instance of the class that is named Default__*, through the
// index and never the object array. Null while the class is not listed yet.
void* FindStaticsCdo() {
    void* const cls = object_index::ClassByName(P::name::GameplayStaticsClass);
    if (!cls) return nullptr;
    void* found = nullptr;
    object_index::ForEachInstance(cls, &OnStaticsInstance, &found);
    return found;
}

// Before the core resolves the pass retries every 2 s; after it, the optional members (origPawn, the sleep verb,
// the dilation getter) are asked again every 30 s until they resolve. No lookup here walks the object array: the
// classes come from the index by name, and the default object from the index's list of its class's instances.
void ResolvePass() {
    if (g_allResolved) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + (g_coreResolved ? std::chrono::seconds(30) : std::chrono::seconds(2));

    if (!g_gmCls) g_gmCls = object_index::ClassByName(L"mainGamemode_C");
    if (!g_gmCls) return;
    if (g_offIsSleep < 0) g_offIsSleep = R::FindPropertyOffset(g_gmCls, L"isSleep");
    if (g_offDreamProb < 0) g_offDreamProb = R::FindPropertyOffset(g_gmCls, L"dreamProbability");
    if (g_offBed < 0) g_offBed = R::FindPropertyOffset(g_gmCls, L"bed");
    if (g_offOrigPawn < 0) g_offOrigPawn = R::FindPropertyOffset(g_gmCls, L"origPawn");
    if (!g_sleepFn) g_sleepFn = R::FindFunction(g_gmCls, L"sleep");

    if (!g_gsCdo) g_gsCdo = FindStaticsCdo();
    if (g_gsCdo) {
        void* gsCls = R::ClassOf(g_gsCdo);
        if (gsCls && !g_setDilationFn)
            g_setDilationFn = R::FindFunction(gsCls, L"SetGlobalTimeDilation");
        if (gsCls && !g_getDilationFn)
            g_getDilationFn = R::FindFunction(gsCls, L"GetGlobalTimeDilation");
    }
    const bool core = g_offIsSleep >= 0 && g_offDreamProb >= 0 && g_offBed >= 0 &&
                      g_setDilationFn;
    if (core && !g_coreResolved) {
        g_coreResolved = true;
        UE_LOGI("sleep: resolved (isSleep=0x%X dreamProb=0x%X bed=0x%X origPawn=0x%X sleep=%s "
                "dilation set/get=%s/%s)",
                g_offIsSleep, g_offDreamProb, g_offBed, g_offOrigPawn,
                g_sleepFn ? "yes" : "NO", g_setDilationFn ? "yes" : "NO",
                g_getDilationFn ? "yes" : "NO");
    }
    g_allResolved = core && g_offOrigPawn >= 0 && g_sleepFn && g_getDilationFn;
}

}  // namespace

bool EnsureResolved() {
    ResolvePass();
    return g_coreResolved;
}

bool IsSleeping() {
    void* gm = world_singleton::Gamemode();
    if (!gm || g_offIsSleep < 0) return false;
    return *(reinterpret_cast<uint8_t*>(gm) + g_offIsSleep) != 0;
}

std::wstring BedClassName() {
    void* gm = world_singleton::Gamemode();
    if (!gm || g_offBed < 0) return std::wstring();
    void* bed = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offBed);
    if (!bed || !R::IsLive(bed)) return std::wstring();
    return R::ClassNameOf(bed);
}

void* OrigPawn() {
    void* gm = world_singleton::Gamemode();
    if (!gm || g_offOrigPawn < 0) return nullptr;
    void* pawn = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offOrigPawn);
    return (pawn && R::IsLive(pawn)) ? pawn : nullptr;
}

bool SetDreamProbability(float v) {
    void* gm = world_singleton::Gamemode();
    if (!gm || g_offDreamProb < 0) return false;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gm) + g_offDreamProb) = v;
    return true;
}

bool SetGlobalTimeDilation(float v) {
    if (!g_gsCdo || !g_setDilationFn) return false;
    void* ctx = ue_wrap::engine::GetWorldContext();
    if (!ctx) return false;
    ue_wrap::ParamFrame f(g_setDilationFn);
    if (!f.valid()) return false;
    f.Set<void*>(L"WorldContextObject", ctx);
    f.Set<float>(L"TimeDilation", v);
    return ue_wrap::Call(g_gsCdo, f);
}

float GetGlobalTimeDilation() {
    if (!g_gsCdo || !g_getDilationFn) return -1.f;
    void* ctx = ue_wrap::engine::GetWorldContext();
    if (!ctx) return -1.f;
    ue_wrap::ParamFrame f(g_getDilationFn);
    if (!f.valid()) return -1.f;
    f.Set<void*>(L"WorldContextObject", ctx);
    if (!ue_wrap::Call(g_gsCdo, f)) return -1.f;
    return f.Get<float>(L"ReturnValue");
}

namespace {

// The PLACED base bed is a cooked SUBCLASS (bed_b_C / bed_m_C / ... -- the 00:17 smoke proved zero exact bed_C
// instances), so a bed is matched by descent from bed_C and never by class name. The one test both finders use.
void* BedBase() { return object_index::ClassByName(L"bed_C"); }
bool IsBedClass(void* cls, void* base) { return cls && base && R::IsDescendantOfAny(cls, &base, 1); }

struct BedPick {
    void* base = nullptr;
    std::vector<void*> classes;
    FVector from;
    void* best = nullptr;
    float bestD2 = 0.f;
};

void OnBedClass(void* ctx, void* cls, void*) {
    auto* p = static_cast<BedPick*>(ctx);
    if (IsBedClass(cls, p->base)) p->classes.push_back(cls);
}

void OnBedInstance(void* ctx, void* obj, int32_t) {
    auto* p = static_cast<BedPick*>(ctx);
    if (IsCdo(obj)) return;
    FVector at;
    if (!engine::TryGetActorLocation(obj, at)) return;
    const float dx = at.X - p->from.X, dy = at.Y - p->from.Y, dz = at.Z - p->from.Z;
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (!p->best || d2 < p->bestD2) {
        p->best = obj;
        p->bestD2 = d2;
    }
}

}  // namespace

void* FindBed() {
    // One-shot walk over the object array; never on a hot path.
    void* const base = BedBase();
    if (!base) return nullptr;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || !R::IsLive(obj)) continue;
        if (!IsBedClass(R::ClassOf(obj), base)) continue;
        if (IsCdo(obj)) continue;
        return obj;
    }
    return nullptr;
}

void* NearestBed(const FVector& from) {
    BedPick pick;
    pick.base = BedBase();
    pick.from = from;
    if (!pick.base) return nullptr;
    object_index::ForEachClass(&OnBedClass, &pick);
    for (void* cls : pick.classes) object_index::ForEachInstance(cls, &OnBedInstance, &pick);
    return pick.best;
}

bool CallSleep(void* bed) {
    void* gm = world_singleton::Gamemode();
    if (!gm || !g_sleepFn || !bed) return false;
    ue_wrap::ParamFrame f(g_sleepFn);
    if (!f.valid()) return false;
    if (!f.Set<void*>(L"bed", bed)) return false;
    f.Set<bool>(L"dropItem", false);
    f.Set<bool>(L"ignoreRagdoll", true);
    return ue_wrap::Call(gm, f);
}

}  // namespace ue_wrap::sleep
