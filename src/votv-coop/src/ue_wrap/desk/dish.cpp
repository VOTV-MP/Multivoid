// ue_wrap/desk/dish.cpp -- see ue_wrap/desk/dish.h.

#include "ue_wrap/desk/dish.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"
#include "coop/text/i18n.h"

#include <chrono>

namespace ue_wrap::dish {
namespace {

namespace R = ue_wrap::reflection;

struct TArrayView { uint8_t* data; int32_t num; int32_t max; };

int32_t g_offDishs = -1;        // mainGamemode_C::dishs (TArray<Adish_C*>)
int32_t g_offActiveDishes = -1; // mainGamemode_C::activeDishes (TArray<bool>)

int32_t g_offLookAt = -1;       // Adish_C::lookAt (FVector -- absolute post-write)
int32_t g_offIsMoving = -1;     // Adish_C::isMoving
int32_t g_offAxisY = -1;        // Adish_C::axis_Y (UBillboardComponent*)
int32_t g_offAxisZ = -1;        // Adish_C::axis_Z (UBillboardComponent*)
int32_t g_offMoveCue = -1;      // Adish_C::satellite_move_Cue
int32_t g_offCue = -1;          // Adish_C::satellite_Cue
int32_t g_offCalibration = -1;  // Adish_C::calibration (float)
int32_t g_offTechName = -1;     // Adish_C::techName (FString)
int32_t g_offHashcode = -1;     // Adish_C::hashcode (FString), outside the L4 set: nothing mirrors it

// Engine-class functions -- resolved on their DECLARING class (FindFunction
// is exact-owner, no SuperStruct climb).
int32_t g_offRelRot = -1;       // USceneComponent::RelativeRotation (raw READ ok)
void* g_setRelRotFn = nullptr;  // USceneComponent::K2_SetRelativeRotation
void* g_activateFn = nullptr;   // UActorComponent::Activate(bool bReset)
void* g_deactivateFn = nullptr; // UActorComponent::Deactivate()
void* g_isActiveFn = nullptr;   // UActorComponent::IsActive() -> bool

// Tickers: singletons the gamemode's BeginPlay creates.
void* g_kismetSysCdo = nullptr;
void* g_clearTimerFn = nullptr;        // KismetSystemLibrary::K2_ClearTimer

std::chrono::steady_clock::time_point g_nextResolve{};
bool g_coreResolved = false;
bool g_l4Resolved = false;

void ResolvePass() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);
    // The classes are looked up each pass, never kept: a member offset outlives its class object, a class pointer
    // need not.
    void* gamemodeCls = object_index::ClassByName(L"mainGamemode_C");
    void* dishCls = object_index::ClassByName(L"dish_C");
    if (!gamemodeCls || !dishCls) return;
    if (g_offDishs < 0) g_offDishs = R::FindPropertyOffset(gamemodeCls, L"dishs");
    if (g_offLookAt < 0) g_offLookAt = R::FindPropertyOffset(dishCls, L"lookAt");
    if (g_offIsMoving < 0) g_offIsMoving = R::FindPropertyOffset(dishCls, L"isMoving");
    const bool core = g_offDishs >= 0 && g_offLookAt >= 0 && g_offIsMoving >= 0;
    if (core && !g_coreResolved) {
        g_coreResolved = true;
        UE_LOGI("dish: resolved (dishs=0x%X lookAt=0x%X isMoving=0x%X)", g_offDishs, g_offLookAt, g_offIsMoving);
    }

    // L4 surface (pose mirror + park + calibration).
    if (g_offActiveDishes < 0)
        g_offActiveDishes = R::FindPropertyOffset(gamemodeCls, L"activeDishes");
    if (g_offAxisY < 0) g_offAxisY = R::FindPropertyOffset(dishCls, L"axis_Y");
    if (g_offAxisZ < 0) g_offAxisZ = R::FindPropertyOffset(dishCls, L"axis_Z");
    if (g_offMoveCue < 0) g_offMoveCue = R::FindPropertyOffset(dishCls, L"satellite_move_Cue");
    if (g_offCue < 0) g_offCue = R::FindPropertyOffset(dishCls, L"satellite_Cue");
    if (g_offCalibration < 0) g_offCalibration = R::FindPropertyOffset(dishCls, L"calibration");
    if (g_offTechName < 0) g_offTechName = R::FindPropertyOffset(dishCls, L"techName");
    if (g_offHashcode < 0) g_offHashcode = R::FindPropertyOffset(dishCls, L"hashcode");
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
    if (!g_kismetSysCdo) g_kismetSysCdo = R::FindClassDefaultObject(L"KismetSystemLibrary");
    if (g_kismetSysCdo && !g_clearTimerFn) {
        if (void* kc = R::FindClass(L"KismetSystemLibrary"))
            g_clearTimerFn = R::FindFunction(kc, L"K2_ClearTimer");
    }
    const bool l4 = g_offActiveDishes >= 0 && g_offAxisY >= 0 && g_offAxisZ >= 0 &&
                    g_offMoveCue >= 0 && g_offCue >= 0 && g_offCalibration >= 0 &&
                    g_offTechName >= 0 && g_offRelRot >= 0 && g_setRelRotFn &&
                    g_activateFn && g_deactivateFn && g_isActiveFn && g_clearTimerFn;
    if (l4 && !g_l4Resolved) {
        g_l4Resolved = true;
        UE_LOGI("dish: L4 surface resolved (axes=0x%X/0x%X cues=0x%X/0x%X activeDishes=0x%X "
                "calib=0x%X)",
                g_offAxisZ, g_offAxisY, g_offMoveCue, g_offCue, g_offActiveDishes,
                g_offCalibration);
    }
}

void* Gamemode() { return world_singleton::Gamemode(); }

TArrayView* Dishs() {
    void* gm = Gamemode();
    if (!gm || g_offDishs < 0) return nullptr;
    return reinterpret_cast<TArrayView*>(reinterpret_cast<uint8_t*>(gm) + g_offDishs);
}

void* DishAt(TArrayView* a, int32_t i) {
    void* d = reinterpret_cast<void**>(a->data)[i];
    return (d && R::IsLive(d)) ? d : nullptr;
}

}  // namespace

void* DishByIndex(int32_t index) {
    TArrayView* a = Dishs();
    if (!a || a->num < 0 || a->num > 64 || index < 0 || index >= a->num) return nullptr;
    return DishAt(a, index);
}

namespace {

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
    if (!f.GetRaw(coop::i18n::TrW(L"ReturnValue"), &active, sizeof(active))) return false;
    ok = true;
    return active;
}

// One live instance of a singleton ticker class, CACHED like Gamemode(): pointer plus
// InternalIndexOf fast path, with the GUObjectArray walk only on a cache miss (boot, or a
// level reload that killed the old instance). Uncached, this cost the client two full walks
// a second at the 1 Hz park latch.
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

int32_t ReadCalibrations(DishCalibration* out, int32_t cap) {
    TArrayView* a = Dishs();
    if (!a || !g_l4Resolved || !out || cap <= 0) return 0;
    if (a->num < 0 || a->num > 64) return 0;
    int32_t n = 0;
    for (int32_t i = 0; i < a->num && n < cap; ++i) {
        void* d = DishAt(a, i);
        if (!d) continue;
        out[n].index = i;
        out[n].value = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offCalibration);
        ++n;
    }
    return n;
}

bool WritePose(int32_t index, float yawZ, float rollY) {
    if (!g_l4Resolved) return false;
    void* d = DishByIndex(index);
    if (!d) return false;
    // The native loop's own channel shape: each write zeroes the component's other two channels.
    // FRotator = {Pitch, Yaw, Roll}.
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

bool WriteCalibration(int32_t index, float v) {
    if (!g_l4Resolved) return false;
    void* d = DishByIndex(index);
    if (!d) return false;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offCalibration) = v;
    return true;
}

void* HitComponent(int32_t index) {
    if (!g_l4Resolved) return nullptr;
    void* d = DishByIndex(index);
    return d ? ComponentAt(d, g_offAxisZ) : nullptr;
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

bool ReadHashDigest(HashDigest& out) {
    out = HashDigest{};
    ResolvePass();
    TArrayView* a = Dishs();
    if (!a || g_offHashcode < 0 || a->num < 0 || a->num > 64) return false;
    uint64_t h = 0xcbf29ce484222325ull;
    const auto mix = [&h](uint8_t byte) { h ^= byte; h *= 0x100000001b3ull; };
    const auto mix32 = [&mix](int32_t v) {
        for (int k = 0; k < 4; ++k) mix(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * k)));
    };
    for (int32_t i = 0; i < a->num; ++i) {
        void* d = DishAt(a, i);
        // Each entry mixes its length first, so the concatenation of two codes cannot collide with
        // a different split of the same characters. A dead entry mixes -1, and a code longer than
        // the bound mixes -2 and is not counted as filled, so neither reads as an empty dish.
        const auto* s = d ? reinterpret_cast<const TArrayView*>(reinterpret_cast<uint8_t*>(d) + g_offHashcode)
                          : nullptr;
        int32_t len = -1;
        if (d) len = (!s || !s->data || s->num <= 1) ? 0 : (s->num > 4096 ? -2 : s->num - 1);
        mix32(len);
        if (len <= 0) continue;
        const auto* w = reinterpret_cast<const wchar_t*>(s->data);
        for (int32_t c = 0; c < len; ++c) {
            mix(static_cast<uint8_t>(w[c] & 0xFF));
            mix(static_cast<uint8_t>((w[c] >> 8) & 0xFF));
        }
        ++out.filled;
    }
    out.digest = h;
    out.dishes = a->num;
    return true;
}

bool ReadHashcode(int32_t index, std::wstring& out) {
    out.clear();
    ResolvePass();
    void* d = DishByIndex(index);
    if (!d || g_offHashcode < 0) return false;
    const auto* s = reinterpret_cast<const TArrayView*>(reinterpret_cast<uint8_t*>(d) + g_offHashcode);
    if (!s->data || s->num <= 1) return true;
    if (s->num - 1 > kMaxHashcodeChars) return false;
    out.assign(reinterpret_cast<const wchar_t*>(s->data), static_cast<size_t>(s->num - 1));
    return true;
}

bool WriteHashcode(int32_t index, const std::wstring& code) {
    ResolvePass();
    void* d = DishByIndex(index);
    if (!d || g_offHashcode < 0 || code.size() > static_cast<size_t>(kMaxHashcodeChars)) return false;
    return ue_wrap::field_io::WriteFStringField(d, g_offHashcode, code);
}

int32_t IndexOf(void* dish) {
    ResolvePass();
    TArrayView* a = Dishs();
    if (!dish || !a || a->num < 0 || a->num > 64) return -1;
    for (int32_t i = 0; i < a->num; ++i)
        if (reinterpret_cast<void**>(a->data)[i] == dish) return i;
    return -1;
}

bool CallCheckFordDishes() {
    void* gm = Gamemode();
    void* fn = gm ? R::FindDispatchFunctionCached(R::ClassOf(gm), L"checkFordDishes") : nullptr;
    return fn && CallNoArg(gm, fn);
}

bool CallSetPrec() {
    void* gm = Gamemode();
    void* fn = gm ? R::FindDispatchFunctionCached(R::ClassOf(gm), kSetPrec) : nullptr;
    return fn && CallNoArg(gm, fn);
}

void* DisherInstance() {
    ResolvePass();
    return SingletonOf(object_index::ClassByName(L"ticker_disher_C"), L"ticker_disher_C", g_disherCache);
}

bool ParkDisher(void* inst) {
    if (!inst || !g_clearTimerFn || !g_kismetSysCdo) return false;
    // K2_ClearTimer(Object, FunctionName="do") -- the exact inverse of the blueprint's one-shot
    // K2_SetTimerDelegate({self, do}) arm, the same shape space_renderer's KillClientSpawnTimer
    // uses.
    ue_wrap::ParamFrame f(g_clearTimerFn);
    if (!f.valid()) return false;
    f.Set<void*>(L"Object", inst);
    const wchar_t* fn = L"do";
    struct { const wchar_t* data; int32_t num; int32_t max; } fs{ fn, 3, 3 };
    if (!f.SetRaw(L"FunctionName", &fs, sizeof(fs))) return false;
    return ue_wrap::Call(g_kismetSysCdo, f);
}

bool RestoreDisher(void* inst) {
    // ReceiveBeginPlay is the native initializer, and re-firing it is safe. Its bytecode runs the
    // parent BeginPlay, gates on the gamemode, then binds a delegate to this object's `do` and
    // arms a one-shot K2_SetTimerDelegate for a random 1800-3600 s. Nothing spawns, `do` itself is
    // never called, and the engine keys a dynamic timer by object and function name, so a second
    // arm re-arms that timer instead of stacking another.
    void* fn = inst ? R::FindDispatchFunctionCached(R::ClassOf(inst), L"ReceiveBeginPlay") : nullptr;
    return fn && CallNoArg(inst, fn);
}

}  // namespace ue_wrap::dish
