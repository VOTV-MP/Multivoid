// ue_wrap/dish.cpp -- see ue_wrap/dish.h.

#include "ue_wrap/desk/dish.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>

namespace ue_wrap::dish {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

struct TArrayView { uint8_t* data; int32_t num; int32_t max; };

void* g_gamemodeCls = nullptr;
void* g_gamemode = nullptr;
int32_t g_gamemodeIdx = -1;
int32_t g_offDishs = -1;        // mainGamemode_C::dishs (@0x0330, TArray<Adish_C*>)
int32_t g_offActiveDishes = -1; // mainGamemode_C::activeDishes (@0x0350, TArray<bool>)

void* g_dishCls = nullptr;
int32_t g_offLookAt = -1;       // Adish_C::lookAt (@0x378, FVector -- absolute post-write)
int32_t g_offIsMoving = -1;     // Adish_C::isMoving (@0x384)
int32_t g_offAxisY = -1;        // Adish_C::axis_Y (@0x300, UBillboardComponent*)
int32_t g_offAxisZ = -1;        // Adish_C::axis_Z (@0x320, UBillboardComponent*)
int32_t g_offMoveCue = -1;      // Adish_C::satellite_move_Cue (@0x308)
int32_t g_offCue = -1;          // Adish_C::satellite_Cue (@0x310)
int32_t g_offCalibration = -1;  // Adish_C::calibration (@0x3AC, float)
int32_t g_offTechName = -1;     // Adish_C::techName (@0x408, FString)
void* g_startMovingToFn = nullptr;  // startMovingTo(lookAt) -- the relative slew entry
void* g_stopFn = nullptr;           // stop() -- 2 flag writes (impl-RE SS2)

// Engine-class functions -- resolved on their DECLARING class (FindFunction
// is exact-owner, no SuperStruct climb).
int32_t g_offRelRot = -1;       // USceneComponent::RelativeRotation (raw READ ok)
void* g_setRelRotFn = nullptr;  // USceneComponent::K2_SetRelativeRotation
void* g_activateFn = nullptr;   // UActorComponent::Activate(bool bReset)
void* g_deactivateFn = nullptr; // UActorComponent::Deactivate()
void* g_isActiveFn = nullptr;   // UActorComponent::IsActive() -> bool

// Tickers (gamemode-BeginPlay singletons, impl-RE SS4).
void* g_disherCls = nullptr;
void* g_uncalibCls = nullptr;
void* g_disherBeginPlayFn = nullptr;   // ticker_disher_C::ReceiveBeginPlay (BP override)
void* g_kismetSysCdo = nullptr;
void* g_clearTimerFn = nullptr;        // KismetSystemLibrary::K2_ClearTimer

std::chrono::steady_clock::time_point g_nextResolve{};
bool g_coreResolved = false;
bool g_l4Resolved = false;

void ResolvePass() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);
    if (!g_gamemodeCls) g_gamemodeCls = R::FindClass(L"mainGamemode_C");
    if (!g_dishCls) g_dishCls = R::FindClass(L"dish_C");
    if (!g_gamemodeCls || !g_dishCls) return;
    if (g_offDishs < 0) g_offDishs = R::FindPropertyOffset(g_gamemodeCls, L"dishs");
    if (g_offLookAt < 0) g_offLookAt = R::FindPropertyOffset(g_dishCls, L"lookAt");
    if (g_offIsMoving < 0) g_offIsMoving = R::FindPropertyOffset(g_dishCls, L"isMoving");
    if (!g_startMovingToFn) g_startMovingToFn = R::FindFunction(g_dishCls, L"startMovingTo");
    const bool core = g_offDishs >= 0 && g_offLookAt >= 0 && g_offIsMoving >= 0 &&
                      g_startMovingToFn != nullptr;
    if (core && !g_coreResolved) {
        g_coreResolved = true;
        UE_LOGI("dish: resolved (dishs=0x%X lookAt=0x%X isMoving=0x%X startMovingTo=yes)",
                g_offDishs, g_offLookAt, g_offIsMoving);
    }

    // L4 surface (pose mirror + park + calibration).
    if (g_offActiveDishes < 0)
        g_offActiveDishes = R::FindPropertyOffset(g_gamemodeCls, L"activeDishes");
    if (g_offAxisY < 0) g_offAxisY = R::FindPropertyOffset(g_dishCls, L"axis_Y");
    if (g_offAxisZ < 0) g_offAxisZ = R::FindPropertyOffset(g_dishCls, L"axis_Z");
    if (g_offMoveCue < 0) g_offMoveCue = R::FindPropertyOffset(g_dishCls, L"satellite_move_Cue");
    if (g_offCue < 0) g_offCue = R::FindPropertyOffset(g_dishCls, L"satellite_Cue");
    if (g_offCalibration < 0) g_offCalibration = R::FindPropertyOffset(g_dishCls, L"calibration");
    if (g_offTechName < 0) g_offTechName = R::FindPropertyOffset(g_dishCls, L"techName");
    if (!g_stopFn) g_stopFn = R::FindFunction(g_dishCls, L"stop");
    if (g_offRelRot < 0 || !g_setRelRotFn) {
        if (void* sc = R::FindClass(L"SceneComponent")) {
            if (g_offRelRot < 0) g_offRelRot = R::FindPropertyOffset(sc, L"RelativeRotation");
            if (!g_setRelRotFn) g_setRelRotFn = R::FindFunction(sc, L"K2_SetRelativeRotation");
        }
    }
    if (!g_activateFn || !g_deactivateFn || !g_isActiveFn) {
        if (void* ac = R::FindClass(L"ActorComponent")) {
            if (!g_activateFn) g_activateFn = R::FindFunction(ac, L"Activate");
            if (!g_deactivateFn) g_deactivateFn = R::FindFunction(ac, L"Deactivate");
            if (!g_isActiveFn) g_isActiveFn = R::FindFunction(ac, L"IsActive");
        }
    }
    if (!g_disherCls) g_disherCls = R::FindClass(L"ticker_disher_C");
    if (!g_uncalibCls) g_uncalibCls = R::FindClass(L"ticker_dishUncalib_C");
    if (g_disherCls && !g_disherBeginPlayFn)
        g_disherBeginPlayFn = R::FindFunction(g_disherCls, L"ReceiveBeginPlay");
    if (!g_kismetSysCdo) g_kismetSysCdo = R::FindClassDefaultObject(L"KismetSystemLibrary");
    if (g_kismetSysCdo && !g_clearTimerFn) {
        if (void* kc = R::FindClass(L"KismetSystemLibrary"))
            g_clearTimerFn = R::FindFunction(kc, L"K2_ClearTimer");
    }
    const bool l4 = g_offActiveDishes >= 0 && g_offAxisY >= 0 && g_offAxisZ >= 0 &&
                    g_offMoveCue >= 0 && g_offCue >= 0 && g_offCalibration >= 0 &&
                    g_offTechName >= 0 && g_stopFn && g_offRelRot >= 0 && g_setRelRotFn &&
                    g_activateFn && g_deactivateFn && g_isActiveFn && g_disherCls &&
                    g_uncalibCls && g_disherBeginPlayFn && g_clearTimerFn;
    if (l4 && !g_l4Resolved) {
        g_l4Resolved = true;
        UE_LOGI("dish: L4 surface resolved (axes=0x%X/0x%X cues=0x%X/0x%X activeDishes=0x%X "
                "calib=0x%X stop=yes tickers=yes)",
                g_offAxisZ, g_offAxisY, g_offMoveCue, g_offCue, g_offActiveDishes,
                g_offCalibration);
    }
}

void* Gamemode() {
    if (g_gamemode && R::IsLiveByIndex(g_gamemode, g_gamemodeIdx)) return g_gamemode;
    g_gamemode = nullptr;
    if (!g_gamemodeCls) return nullptr;
    for (void* obj : R::FindObjectsByClass(L"mainGamemode_C")) {
        if (obj && R::IsLive(obj)) {
            g_gamemode = obj;
            g_gamemodeIdx = R::InternalIndexOf(obj);
            break;
        }
    }
    return g_gamemode;
}

TArrayView* Dishs() {
    void* gm = Gamemode();
    if (!gm || g_offDishs < 0) return nullptr;
    return reinterpret_cast<TArrayView*>(reinterpret_cast<uint8_t*>(gm) + g_offDishs);
}

void* DishAt(TArrayView* a, int32_t i) {
    void* d = reinterpret_cast<void**>(a->data)[i];
    return (d && R::IsLive(d)) ? d : nullptr;
}

void* DishByIndex(int32_t index) {
    TArrayView* a = Dishs();
    if (!a || a->num < 0 || a->num > 64 || index < 0 || index >= a->num) return nullptr;
    return DishAt(a, index);
}

void* ComponentAt(void* d, int32_t off) {
    if (!d || off < 0) return nullptr;
    void* c = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(d) + off);
    return (c && R::IsLive(c)) ? c : nullptr;
}

bool CallNoArg(void* obj, void* fn) {
    if (!obj || !fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    return ue_wrap::Call(obj, f);
}

bool SetRelRot(void* comp, const ue_wrap::FRotator& rot) {
    if (!comp || !g_setRelRotFn) return false;
    ue_wrap::ParamFrame f(g_setRelRotFn);
    if (!f.valid()) return false;
    f.Set<ue_wrap::FRotator>(L"NewRotation", rot);
    f.Set<bool>(L"bSweep", false);
    f.Set<bool>(L"bTeleport", true);
    return ue_wrap::Call(comp, f);  // SweepHitResult out param zero-filled by the frame
}

// Reflected UActorComponent::IsActive() on one cue; ok=false on a failed read.
bool CueIsActive(void* cue, bool& ok) {
    ok = false;
    if (!cue || !g_isActiveFn) return false;
    ue_wrap::ParamFrame f(g_isActiveFn);
    if (!f.valid()) return false;
    if (!ue_wrap::Call(cue, f)) return false;
    bool active = false;
    if (!f.GetRaw(L"ReturnValue", &active, sizeof(active))) return false;
    ok = true;
    return active;
}

// One live instance of a singleton ticker class, CACHED like Gamemode():
// ptr + InternalIndexOf fast path; the GUObjectArray walk runs only on a
// cache miss (boot / level reload killed the old instance). Perf audit
// 2026-07-16: the uncached form cost the client 2 full walks/s at the 1 Hz
// park latch.
struct SingletonCache { void* obj = nullptr; int32_t idx = -1; };

void* SingletonOf(void* cls, const wchar_t* clsName, SingletonCache& cache) {
    if (cache.obj && R::IsLiveByIndex(cache.obj, cache.idx)) return cache.obj;
    cache = {};
    if (!cls) return nullptr;
    for (void* obj : R::FindObjectsByClass(clsName)) {
        if (obj && R::IsLive(obj) &&
            !R::NameStartsWith(R::NameOf(obj), L"Default__")) {
            cache.obj = obj;
            cache.idx = R::InternalIndexOf(obj);
            return obj;
        }
    }
    return nullptr;
}

SingletonCache g_disherCache;
SingletonCache g_uncalibCache;

}  // namespace

bool EnsureResolved() {
    ResolvePass();
    return g_coreResolved && g_l4Resolved;
}

int32_t Count() {
    TArrayView* a = Dishs();
    if (!a || a->num < 0 || a->num > 64) return 0;  // sanity (the map places ~10)
    return a->num;
}

int32_t MovingCount() {
    TArrayView* a = Dishs();
    if (!a || !g_coreResolved) return -1;
    if (a->num < 0 || a->num > 64) return -1;
    int32_t moving = 0;
    for (int32_t i = 0; i < a->num; ++i) {
        void* d = DishAt(a, i);
        if (d && *(reinterpret_cast<uint8_t*>(d) + g_offIsMoving)) ++moving;
    }
    return moving;
}

int32_t ReadAllDishStates(DishState* out, int32_t cap) {
    TArrayView* a = Dishs();
    if (!a || !g_coreResolved || !out || cap <= 0) return 0;
    if (a->num < 0 || a->num > 64) return 0;
    int32_t n = 0;
    for (int32_t i = 0; i < a->num && n < cap; ++i) {
        void* d = DishAt(a, i);
        if (!d) continue;
        // lookAt = the absolute commanded TARGET (rewritten param+ActorLocation
        // at startMovingTo). It is the SETTLED per-dish discriminator: readable
        // WHILE isMoving=true, so a diff can compare aim targets mid-slew.
        const auto* la = reinterpret_cast<const float*>(
            reinterpret_cast<uint8_t*>(d) + g_offLookAt);
        out[n].index    = i;
        out[n].lookAtX  = la[0];
        out[n].lookAtY  = la[1];
        out[n].lookAtZ  = la[2];
        out[n].isMoving = *(reinterpret_cast<uint8_t*>(d) + g_offIsMoving) != 0;
        ++n;
    }
    return n;
}

bool ReadSlewFromMovingDish(ue_wrap::FVector& out) {
    TArrayView* a = Dishs();
    if (!a || !g_coreResolved) return false;
    if (a->num < 0 || a->num > 64) return false;
    // Moving dish preferred; else FIRST live dish. v116 fallback (qf R5-Q1):
    // the catch chain writes lookAt ABSOLUTE to ALL dishes at the catch moment
    // (impl-RE SS8 loop), so within the detector's <=1 s poll window a fully
    // SETTLED array still holds the fresh target -- without the fallback a
    // near-aim catch shipped slewValid=0 and the host never armed (no theater
    // -> no dishesStop -> no formDownload).
    void* pick = nullptr;
    for (int32_t i = 0; i < a->num; ++i) {
        void* d = DishAt(a, i);
        if (!d) continue;
        if (!pick) pick = d;
        if (*(reinterpret_cast<uint8_t*>(d) + g_offIsMoving)) { pick = d; break; }
    }
    if (!pick) return false;
    // lookAt was rewritten absolute at startMovingTo (@162: param +
    // ActorLocation); subtracting the dish's own location recovers the
    // shared relative vector the catch chain passed to every dish.
    const auto* la = reinterpret_cast<const float*>(
        reinterpret_cast<uint8_t*>(pick) + g_offLookAt);
    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(pick);
    out.X = la[0] - loc.X;
    out.Y = la[1] - loc.Y;
    out.Z = la[2] - loc.Z;
    return true;
}

int32_t StartMovingAll(const ue_wrap::FVector& slew) {
    TArrayView* a = Dishs();
    if (!a || !g_coreResolved) return 0;
    if (a->num < 0 || a->num > 64) return 0;
    int32_t dispatched = 0;
    for (int32_t i = 0; i < a->num; ++i) {
        void* d = DishAt(a, i);
        if (!d) continue;
        ue_wrap::ParamFrame f(g_startMovingToFn);
        if (!f.valid()) return dispatched;
        struct { float X, Y, Z; } v{ slew.X, slew.Y, slew.Z };
        if (!f.SetRaw(L"lookAt", &v, sizeof(v))) return dispatched;
        if (ue_wrap::Call(d, f)) ++dispatched;
    }
    return dispatched;
}

int32_t ReadAllRows(DishRow* out, int32_t cap) {
    TArrayView* a = Dishs();
    if (!a || !g_l4Resolved || !out || cap <= 0) return 0;
    if (a->num < 0 || a->num > 64) return 0;
    int32_t n = 0;
    for (int32_t i = 0; i < a->num && n < cap; ++i) {
        void* d = DishAt(a, i);
        if (!d) continue;
        out[n].index = i;
        out[n].isMoving = *(reinterpret_cast<uint8_t*>(d) + g_offIsMoving) != 0;
        out[n].calibration = *reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(d) + g_offCalibration);
        // FRotator is {Pitch, Yaw, Roll} floats; raw READS are fine (only
        // writes need the K2 pipeline).
        out[n].yawZ = 0.f;
        out[n].rollY = 0.f;
        if (void* az = ComponentAt(d, g_offAxisZ)) {
            const auto* r = reinterpret_cast<const float*>(
                reinterpret_cast<uint8_t*>(az) + g_offRelRot);
            out[n].yawZ = r[1];
        }
        if (void* ay = ComponentAt(d, g_offAxisY)) {
            const auto* r = reinterpret_cast<const float*>(
                reinterpret_cast<uint8_t*>(ay) + g_offRelRot);
            out[n].rollY = r[2];
        }
        ++n;
    }
    return n;
}

bool WritePose(int32_t index, float yawZ, float rollY) {
    if (!g_l4Resolved) return false;
    void* d = DishByIndex(index);
    if (!d) return false;
    // The native loop's own channel shape (impl-RE SS1): each write zeroes the
    // component's other two channels. FRotator = {Pitch, Yaw, Roll}.
    bool ok = true;
    if (void* az = ComponentAt(d, g_offAxisZ))
        ok &= SetRelRot(az, ue_wrap::FRotator{0.f, yawZ, 0.f});
    else ok = false;
    if (void* ay = ComponentAt(d, g_offAxisY))
        ok &= SetRelRot(ay, ue_wrap::FRotator{0.f, 0.f, rollY});
    else ok = false;
    return ok;
}

bool WriteIsMoving(int32_t index, bool moving) {
    if (!g_coreResolved) return false;
    void* d = DishByIndex(index);
    if (!d) return false;
    *(reinterpret_cast<uint8_t*>(d) + g_offIsMoving) = moving ? 1 : 0;
    return true;
}

bool StopDish(int32_t index) {
    if (!g_l4Resolved) return false;
    void* d = DishByIndex(index);
    return d && CallNoArg(d, g_stopFn);
}

bool DeactivateCues(int32_t index) {
    if (!g_l4Resolved) return false;
    void* d = DishByIndex(index);
    if (!d) return false;
    bool ok = true;
    if (void* c = ComponentAt(d, g_offMoveCue)) ok &= CallNoArg(c, g_deactivateFn);
    if (void* c = ComponentAt(d, g_offCue)) ok &= CallNoArg(c, g_deactivateFn);
    return ok;
}

bool ActivateMoveCue(int32_t index) {
    if (!g_l4Resolved) return false;
    void* d = DishByIndex(index);
    if (!d) return false;
    void* c = ComponentAt(d, g_offMoveCue);
    if (!c || !g_activateFn) return false;
    ue_wrap::ParamFrame f(g_activateFn);
    if (!f.valid()) return false;
    f.Set<bool>(L"bReset", false);
    return ue_wrap::Call(c, f);
}

bool AnyCueActive(int32_t index, bool& ok) {
    ok = false;
    if (!g_l4Resolved) return false;
    void* d = DishByIndex(index);
    if (!d) return false;
    bool ok1 = false, ok2 = false;
    const bool a1 = CueIsActive(ComponentAt(d, g_offMoveCue), ok1);
    const bool a2 = CueIsActive(ComponentAt(d, g_offCue), ok2);
    ok = ok1 || ok2;
    return (ok1 && a1) || (ok2 && a2);
}

bool ReadActiveDish(int32_t index, bool& out) {
    void* gm = Gamemode();
    if (!gm || g_offActiveDishes < 0) return false;
    auto* a = reinterpret_cast<TArrayView*>(reinterpret_cast<uint8_t*>(gm) + g_offActiveDishes);
    if (!a->data || index < 0 || index >= a->num || a->num > 64) return false;
    out = a->data[index] != 0;
    return true;
}

bool WriteActiveDish(int32_t index, bool active) {
    void* gm = Gamemode();
    if (!gm || g_offActiveDishes < 0) return false;
    auto* a = reinterpret_cast<TArrayView*>(reinterpret_cast<uint8_t*>(gm) + g_offActiveDishes);
    if (!a->data || index < 0 || index >= a->num || a->num > 64) return false;
    a->data[index] = active ? 1 : 0;
    return true;
}

bool ReadCalibration(int32_t index, float& out) {
    if (!g_l4Resolved) return false;
    void* d = DishByIndex(index);
    if (!d) return false;
    out = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offCalibration);
    return true;
}

bool WriteCalibration(int32_t index, float v) {
    if (!g_l4Resolved) return false;
    void* d = DishByIndex(index);
    if (!d) return false;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offCalibration) = v;
    return true;
}

std::wstring TechName(int32_t index) {
    if (!g_l4Resolved) return L"?";
    void* d = DishByIndex(index);
    if (!d) return L"?";
    // FString = {wchar_t* data, int32 num, int32 max}; num includes the NUL.
    const auto* s = reinterpret_cast<const TArrayView*>(
        reinterpret_cast<uint8_t*>(d) + g_offTechName);
    if (!s->data || s->num <= 1 || s->num > 128) return L"?";
    return std::wstring(reinterpret_cast<const wchar_t*>(s->data),
                        static_cast<size_t>(s->num - 1));
}

bool CallCheckFordDishes() {
    void* gm = Gamemode();
    if (!gm || !g_gamemodeCls) return false;
    static void* sFn = nullptr;
    if (!sFn) sFn = R::FindFunction(g_gamemodeCls, L"checkFordDishes");
    return sFn && CallNoArg(gm, sFn);
}

void* DisherInstance() {
    ResolvePass();
    return SingletonOf(g_disherCls, L"ticker_disher_C", g_disherCache);
}

void* UncalibInstance() {
    ResolvePass();
    return SingletonOf(g_uncalibCls, L"ticker_dishUncalib_C", g_uncalibCache);
}

bool ParkDisher(void* inst) {
    if (!inst || !g_clearTimerFn || !g_kismetSysCdo) return false;
    // K2_ClearTimer(Object, FunctionName="do") -- the exact inverse of the
    // BP's one-shot K2_SetTimerDelegate({self, do}) arm (impl-RE SS4;
    // space_renderer KillClientSpawnTimer precedent).
    ue_wrap::ParamFrame f(g_clearTimerFn);
    if (!f.valid()) return false;
    f.Set<void*>(L"Object", inst);
    const wchar_t* fn = L"do";
    struct { const wchar_t* data; int32_t num; int32_t max; } fs{ fn, 3, 3 };
    if (!f.SetRaw(L"FunctionName", &fs, sizeof(fs))) return false;
    return ue_wrap::Call(g_kismetSysCdo, f);
}

bool RestoreDisher(void* inst) {
    // PE ReceiveBeginPlay = the native initializer: gamemode gate ->
    // BindDelegate(LOCAL var) -> Random(1800,3600) -> K2_SetTimerDelegate ->
    // parent BeginPlay. Bytecode-verified re-fire-safe (no do() call, no
    // spawn, local-var bind can't stack; qf R5/R8 2026-07-16).
    return inst && CallNoArg(inst, g_disherBeginPlayFn);
}

bool ParkUncalib(void* inst) {
    if (!inst) return false;
    return E::SetActorTickEnabled(inst, false);
}

bool RestoreUncalib(void* inst) {
    if (!inst) return false;
    return E::SetActorTickEnabled(inst, true);
}

}  // namespace ue_wrap::dish
