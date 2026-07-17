// ue_wrap/tape_caddy.cpp -- see ue_wrap/tape_caddy.h. Offsets resolved from the
// live classes via reflection (version-portable); the Alpha 0.9.0-n values are
// logged fallbacks (wallunit reelBig 0x0288 / reelSmall 0x028C; Aprop_reel_C
// Progress 0x0364).

#include "ue_wrap/desk/tape_caddy.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstdint>

namespace ue_wrap::tape_caddy {
namespace {

namespace R = reflection;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Resolved once (GUObjectArray class entries don't unload in shipped UE4).
void*   g_unitCls     = nullptr;  // Awallunit_tapes_C
void*   g_reelBaseCls = nullptr;  // Aprop_reel_C (declares Progress)
int32_t g_offReelBig   = -1;
int32_t g_offReelSmall = -1;
int32_t g_offProgress  = -1;
void*   g_updFn        = nullptr;
bool    g_resolved     = false;

// The single placed unit (ptr + InternalIndexOf; IsLiveByIndex fast path; the
// GUObjectArray walk runs only on a cache miss -- dish.cpp SingletonCache shape).
void*   g_unit    = nullptr;
int32_t g_unitIdx = -1;

}  // namespace

// Failure backoff: EnsureResolved is called from HOT paths (every PropSpawn express fill
// via prop::ReadSavedScalarForClass -- the join snapshot expresses THOUSANDS of props). A
// failing resolve does 2 GUObjectArray FindClass walks; unthrottled, a world where
// prop_reel_C is not yet loaded (BP classes load on demand) turns the snapshot into
// thousands of walks (the install-loop-bomb class). Retry at most 1/s.
uint64_t g_nextResolveTryMs = 0;

bool EnsureResolved() {
    if (g_resolved) return true;
    const uint64_t now = NowMs();
    if (now < g_nextResolveTryMs) return false;
    g_nextResolveTryMs = now + 1000;
    void* unitCls = R::FindClass(L"wallunit_tapes_C");
    void* reelCls = R::FindClass(L"prop_reel_C");
    if (!unitCls || !reelCls) return false;  // classes not loaded yet -- backoff above throttles
    int32_t big   = R::FindPropertyOffset(unitCls, L"reelBig");
    int32_t small = R::FindPropertyOffset(unitCls, L"reelSmall");
    int32_t prog  = R::FindPropertyOffset(reelCls, L"Progress");
    if (big < 0)   { UE_LOGW("tape_caddy: reelBig offset not found -- fallback 0x0288");   big = 0x0288; }
    if (small < 0) { UE_LOGW("tape_caddy: reelSmall offset not found -- fallback 0x028C"); small = 0x028C; }
    if (prog < 0)  { UE_LOGW("tape_caddy: reel Progress offset not found -- fallback 0x0364"); prog = 0x0364; }
    void* updFn = R::FindFunction(unitCls, L"upd");
    if (!updFn) UE_LOGW("tape_caddy: upd() not found on wallunit_tapes_C -- slot repaint disabled");
    g_unitCls = unitCls; g_reelBaseCls = reelCls;
    g_offReelBig = big; g_offReelSmall = small; g_offProgress = prog;
    g_updFn = updFn;
    g_resolved = true;
    UE_LOGI("tape_caddy: resolved (reelBig=0x%X reelSmall=0x%X Progress=0x%X upd=%p)",
            g_offReelBig, g_offReelSmall, g_offProgress, g_updFn);
    return true;
}

void* Instance() {
    if (!g_resolved) return nullptr;
    if (g_unit && R::IsLiveByIndex(g_unit, g_unitIdx)) return g_unit;
    g_unit = nullptr; g_unitIdx = -1;
    for (void* obj : R::FindObjectsByClass(L"wallunit_tapes_C")) {
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) {
            g_unit = obj;
            g_unitIdx = R::InternalIndexOf(obj);
            return obj;
        }
    }
    return nullptr;
}

bool ReadReels(float& big, float& small) {
    void* u = Instance();
    if (!u) return false;
    big   = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(u) + g_offReelBig);
    small = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(u) + g_offReelSmall);
    return true;
}

bool WriteReel(int reel, float value) {
    void* u = Instance();
    if (!u) return false;
    const int32_t off = (reel == kReelBig) ? g_offReelBig : g_offReelSmall;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(u) + off) = value;
    return true;
}

bool CallUpd() {
    void* u = Instance();
    if (!u || !g_updFn) return false;
    ue_wrap::ParamFrame f(g_updFn);
    if (!f.valid()) return false;
    return ue_wrap::Call(u, f);
}

bool IsReelClass(void* cls) {
    if (!cls || !g_reelBaseCls) return false;
    if (cls == g_reelBaseCls) return true;
    void* base[1] = { g_reelBaseCls };
    return R::IsDescendantOfAny(cls, base, 1);
}

bool ReadProgress(void* reelActor, float& out) {
    if (!reelActor || g_offProgress < 0) return false;
    out = *reinterpret_cast<const float*>(
        reinterpret_cast<const uint8_t*>(reelActor) + g_offProgress);
    return true;
}

bool WriteProgress(void* reelActor, float value) {
    if (!reelActor || g_offProgress < 0) return false;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(reelActor) + g_offProgress) = value;
    return true;
}

void ResetCache() {
    g_unit = nullptr;
    g_unitIdx = -1;
}

}  // namespace ue_wrap::tape_caddy
