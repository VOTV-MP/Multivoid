// ue_wrap/dish.cpp -- see ue_wrap/dish.h.

#include "ue_wrap/dish.h"

#include "ue_wrap/call.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <chrono>

namespace ue_wrap::dish {
namespace {

namespace R = ue_wrap::reflection;

struct TArrayView { uint8_t* data; int32_t num; int32_t max; };

void* g_gamemodeCls = nullptr;
void* g_gamemode = nullptr;
int32_t g_gamemodeIdx = -1;
int32_t g_offDishs = -1;        // mainGamemode_C::dishs (@0x0330, TArray<Adish_C*>)

void* g_dishCls = nullptr;
int32_t g_offLookAt = -1;       // Adish_C::lookAt (@0x378, FVector -- absolute post-write)
int32_t g_offIsMoving = -1;     // Adish_C::isMoving (@0x384)
void* g_startMovingToFn = nullptr;  // startMovingTo(lookAt) -- the relative slew entry

std::chrono::steady_clock::time_point g_nextResolve{};
bool g_coreResolved = false;

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

}  // namespace

bool EnsureResolved() {
    ResolvePass();
    return g_coreResolved;
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
    for (int32_t i = 0; i < a->num; ++i) {
        void* d = DishAt(a, i);
        if (!d || !*(reinterpret_cast<uint8_t*>(d) + g_offIsMoving)) continue;
        // lookAt was rewritten absolute at startMovingTo (@162: param +
        // ActorLocation); subtracting the dish's own location recovers the
        // shared relative vector the catch chain passed to every dish.
        const auto* la = reinterpret_cast<const float*>(
            reinterpret_cast<uint8_t*>(d) + g_offLookAt);
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(d);
        out.X = la[0] - loc.X;
        out.Y = la[1] - loc.Y;
        out.Z = la[2] - loc.Z;
        return true;
    }
    return false;
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

}  // namespace ue_wrap::dish
