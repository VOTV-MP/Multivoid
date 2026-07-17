// ue_wrap/skysphere.cpp -- see ue_wrap/skysphere.h. Engine access for the night-sky actor
// (Anewsky_C). Offsets are resolved from the live class via reflection; the Alpha 0.9.0-n
// values are logged fallbacks. Mirrors ue_wrap/daynightcycle.cpp's cache + resolve shape.

#include "ue_wrap/world/skysphere.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ue_wrap::skysphere {
namespace {

namespace R = reflection;
namespace E = engine;

std::atomic<bool> g_resolved{false};
void*   g_skyCls         = nullptr;  // newsky_C UClass
int32_t g_skyCompOff     = -1;       // Anewsky_C::sky (UStaticMeshComponent*) (Alpha 0.9.0-n: 0x0250)
int32_t g_moonPhaseOff   = -1;       // Anewsky_C::moonPhase_mirror (float)         (0x02BC)
void*   g_setMoonPhaseFn = nullptr;  // setMoonPhase() -- re-applies the moon material param (optional)

constexpr int32_t kSkyCompOffFallback   = 0x0250;
constexpr int32_t kMoonPhaseOffFallback = 0x02BC;

void* g_skyCache = nullptr;  // cached singleton (GT-only)

// The `sky` UStaticMeshComponent pointer off the actor -- its WORLD rotation IS the visible
// star orientation. nullptr if unresolved / not present.
void* SkyComponent(void* newsky) {
    if (!newsky || g_skyCompOff < 0) return nullptr;
    return *reinterpret_cast<void* const*>(
        reinterpret_cast<const char*>(newsky) + g_skyCompOff);
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;
    void* cls = R::FindClass(L"newsky_C");
    if (!cls) return false;  // not loaded yet -- caller retries

    int32_t skyOff = R::FindPropertyOffset(cls, L"sky");
    if (skyOff < 0) {
        UE_LOGW("skysphere: reflected sky offset not found -- using fallback 0x%04X", kSkyCompOffFallback);
        skyOff = kSkyCompOffFallback;
    }
    int32_t moonOff = R::FindPropertyOffset(cls, L"moonPhase_mirror");
    if (moonOff < 0) {
        UE_LOGW("skysphere: reflected moonPhase_mirror offset not found -- using fallback 0x%04X", kMoonPhaseOffFallback);
        moonOff = kMoonPhaseOffFallback;
    }

    g_skyCls         = cls;
    g_skyCompOff     = skyOff;
    g_moonPhaseOff   = moonOff;
    g_setMoonPhaseFn = R::FindFunction(cls, L"setMoonPhase");  // optional -- immediate material refresh on apply
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("skysphere: resolved newsky_C=%p sky@0x%04X moonPhase_mirror@0x%04X setMoonPhase=%p",
            cls, skyOff, moonOff, g_setMoonPhaseFn);
    return true;
}

void* Sky() {
    if (g_skyCache && R::IsLive(g_skyCache)) return g_skyCache;  // steady-state: a pointer check
    // The sky actor is a singleton that, once found, stays live. THROTTLE the GUObjectArray
    // scan to once/sec so a transient miss can never become a per-call walk (the standing
    // per-frame-FindObjectByClass ban). Game-thread-only -> the static is unguarded.
    static std::chrono::steady_clock::time_point s_lastScan{};
    const auto now = std::chrono::steady_clock::now();
    if (now - s_lastScan < std::chrono::seconds(1)) return nullptr;
    s_lastScan = now;
    if (!EnsureResolved()) return nullptr;
    g_skyCache = R::FindObjectByClass(L"newsky_C");
    return (g_skyCache && R::IsLive(g_skyCache)) ? g_skyCache : nullptr;
}

bool ReadSky(FRotator& skyWorldRot, float& moonPhase) {
    void* sky = Sky();
    if (!sky || g_moonPhaseOff < 0) return false;
    void* comp = SkyComponent(sky);
    if (!comp) return false;
    skyWorldRot = E::GetComponentWorldRotation(comp);
    moonPhase = *reinterpret_cast<const float*>(
        reinterpret_cast<const char*>(sky) + g_moonPhaseOff);
    return true;
}

void ApplySky(const FRotator& skyWorldRot, float moonPhase) {
    void* sky = Sky();
    if (!sky || g_moonPhaseOff < 0) return;
    if (void* comp = SkyComponent(sky)) E::SetComponentWorldRotation(comp, skyWorldRot);
    *reinterpret_cast<float*>(reinterpret_cast<char*>(sky) + g_moonPhaseOff) = moonPhase;
    // Re-apply the moon material param so the phase shows immediately (the BP otherwise only
    // refreshes it inside upd()). Optional -- a no-op if setMoonPhase didn't resolve.
    if (g_setMoonPhaseFn) {
        ParamFrame f(g_setMoonPhaseFn);
        if (f.valid()) Call(sky, f);
    }
}

}  // namespace ue_wrap::skysphere
