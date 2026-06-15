// ue_wrap/sleep.cpp -- see ue_wrap/sleep.h.

#include "ue_wrap/sleep.h"

#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"  // GetWorldContext
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <chrono>

namespace ue_wrap::sleep {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

void* g_gmCls = nullptr;
void* g_gm = nullptr;
int32_t g_gmIdx = -1;

int32_t g_offIsSleep = -1;     // mainGamemode.isSleep        @0x04EC
int32_t g_offDreamProb = -1;   // mainGamemode.dreamProbability @0x1030 (-1 sentinel)
int32_t g_offSaveSlot = -1;    // mainGamemode.saveSlot (UsaveSlot_C*)
void* g_wakeupFn = nullptr;    // gamemode.wakeup() -- the timelapse END
void* g_sleepFn = nullptr;     // gamemode.sleep(bed, dropItem, ignoreRagdoll)

void* g_saveSlotCls = nullptr;
int32_t g_offSleepNeed = -1;   // saveSlot.sleep (the 0..100 need)

void* g_gsCdo = nullptr;            // GameplayStatics CDO
void* g_setDilationFn = nullptr;    // SetGlobalTimeDilation(WorldContextObject, TimeDilation)
void* g_getDilationFn = nullptr;    // GetGlobalTimeDilation(WorldContextObject) -> float

void* g_bedCls = nullptr;           // bed_C (probe helper)

int32_t g_offSleepCam = -1;         // mainGamemode.sleepCam     @0x04F0
int32_t g_offSleepingPawn = -1;     // mainGamemode.sleepingPawn @0x1258

std::chrono::steady_clock::time_point g_nextResolve{};
bool g_coreResolved = false;

void ResolvePass() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);

    if (!g_gmCls) g_gmCls = R::FindClass(L"mainGamemode_C");
    if (!g_gmCls) return;
    if (g_offIsSleep < 0) g_offIsSleep = R::FindPropertyOffset(g_gmCls, L"isSleep");
    if (g_offDreamProb < 0) g_offDreamProb = R::FindPropertyOffset(g_gmCls, L"dreamProbability");
    if (g_offSaveSlot < 0) g_offSaveSlot = R::FindPropertyOffset(g_gmCls, L"saveSlot");
    if (!g_wakeupFn) g_wakeupFn = R::FindFunction(g_gmCls, L"wakeup");
    if (!g_sleepFn) g_sleepFn = R::FindFunction(g_gmCls, L"sleep");

    if (!g_saveSlotCls) g_saveSlotCls = R::FindClass(L"saveSlot_C");
    if (g_saveSlotCls && g_offSleepNeed < 0)
        g_offSleepNeed = R::FindPropertyOffset(g_saveSlotCls, L"sleep");

    if (!g_gsCdo) g_gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_gsCdo) {
        void* gsCls = R::ClassOf(g_gsCdo);
        if (gsCls && !g_setDilationFn)
            g_setDilationFn = R::FindFunction(gsCls, L"SetGlobalTimeDilation");
        if (gsCls && !g_getDilationFn)
            g_getDilationFn = R::FindFunction(gsCls, L"GetGlobalTimeDilation");
    }
    if (!g_bedCls) g_bedCls = R::FindClass(L"bed_C");  // optional (probe only)
    // Optional camera surface (the WAITING-state view hold) -- never gates core.
    if (g_offSleepCam < 0) g_offSleepCam = R::FindPropertyOffset(g_gmCls, L"sleepCam");
    if (g_offSleepingPawn < 0)
        g_offSleepingPawn = R::FindPropertyOffset(g_gmCls, L"sleepingPawn");

    const bool core = g_offIsSleep >= 0 && g_offDreamProb >= 0 && g_offSaveSlot >= 0 &&
                      g_wakeupFn && g_offSleepNeed >= 0 && g_setDilationFn;
    if (core && !g_coreResolved) {
        g_coreResolved = true;
        UE_LOGI("sleep: resolved (isSleep=0x%X dreamProb=0x%X saveSlot=0x%X sleepNeed=0x%X "
                "wakeup=%s sleep=%s dilation set/get=%s/%s bedCls=%s)",
                g_offIsSleep, g_offDreamProb, g_offSaveSlot, g_offSleepNeed,
                g_wakeupFn ? "yes" : "NO", g_sleepFn ? "yes" : "NO",
                g_setDilationFn ? "yes" : "NO", g_getDilationFn ? "yes" : "NO",
                g_bedCls ? "yes" : "NO");
    }
}

void* SaveSlot() {
    void* gm = Gamemode();
    if (!gm || g_offSaveSlot < 0) return nullptr;
    void* ss = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offSaveSlot);
    return (ss && R::IsLive(ss)) ? ss : nullptr;
}

}  // namespace

bool EnsureResolved() {
    ResolvePass();
    return g_coreResolved;
}

void* Gamemode() {
    if (g_gm && R::IsLiveByIndex(g_gm, g_gmIdx)) return g_gm;
    g_gm = nullptr;
    if (!g_gmCls) return nullptr;
    for (void* obj : R::FindObjectsByClass(L"mainGamemode_C")) {
        if (obj && R::IsLive(obj)) {
            g_gm = obj;
            g_gmIdx = R::InternalIndexOf(obj);
            break;
        }
    }
    return g_gm;
}

bool IsSleeping() {
    void* gm = Gamemode();
    if (!gm || g_offIsSleep < 0) return false;
    return *(reinterpret_cast<uint8_t*>(gm) + g_offIsSleep) != 0;
}

bool SetDreamProbability(float v) {
    void* gm = Gamemode();
    if (!gm || g_offDreamProb < 0) return false;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gm) + g_offDreamProb) = v;
    return true;
}

bool CallWakeup() {
    void* gm = Gamemode();
    if (!gm || !g_wakeupFn) return false;
    ue_wrap::ParamFrame f(g_wakeupFn);
    if (!f.valid()) return false;
    return ue_wrap::Call(gm, f);
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

bool ReadSleepNeed(float& out) {
    void* ss = SaveSlot();
    if (!ss || g_offSleepNeed < 0) return false;
    out = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ss) + g_offSleepNeed);
    return true;
}

bool WriteSleepNeed(float v) {
    void* ss = SaveSlot();
    if (!ss || g_offSleepNeed < 0) return false;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ss) + g_offSleepNeed) = v;
    return true;
}

namespace {
void* GmActorField(int32_t off) {
    void* gm = Gamemode();
    if (!gm || off < 0) return nullptr;
    void* a = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + off);
    return (a && R::IsLive(a)) ? a : nullptr;
}
}  // namespace

void* SleepCam() { return GmActorField(g_offSleepCam); }
void* SleepingPawn() { return GmActorField(g_offSleepingPawn); }

bool SetSleepViewTarget(void* target) {
    if (!target) return false;
    void* pawn = SleepingPawn();
    if (!pawn) return false;
    void* pc = ue_wrap::engine::GetController(pawn);
    if (!pc) return false;
    return ue_wrap::engine::SetViewTargetWithBlend(pc, target, 0.25f);
}

void* FindBed() {
    // The PLACED base bed is a cooked SUBCLASS (bed_b_C / bed_m_C / ... --
    // the 00:17 smoke proved zero exact bed_C instances), so match by
    // descendant-of-bed_C over the object array instead of by class name.
    // Probe-only one-shot walk; never on a hot path.
    if (!g_bedCls) return nullptr;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || !R::IsLive(obj)) continue;
        void* cls = R::ClassOf(obj);
        if (!cls || !R::IsDescendantOfAny(cls, &g_bedCls, 1)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // CDOs
        return obj;
    }
    return nullptr;
}

bool CallSleep(void* bed) {
    void* gm = Gamemode();
    if (!gm || !g_sleepFn || !bed) return false;
    ue_wrap::ParamFrame f(g_sleepFn);
    if (!f.valid()) return false;
    if (!f.Set<void*>(L"bed", bed)) return false;
    f.Set<bool>(L"dropItem", false);
    f.Set<bool>(L"ignoreRagdoll", true);
    return ue_wrap::Call(gm, f);
}

}  // namespace ue_wrap::sleep
