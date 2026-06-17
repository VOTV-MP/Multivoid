// ue_wrap/daynightcycle.cpp -- see ue_wrap/daynightcycle.h. Engine access for VOTV's
// world clock (AdaynightCycle_C).
//
// Offsets are resolved from the live class via reflection (FindPropertyOffset); the known
// Alpha 0.9.0-n offsets are a logged fallback if the reflected walk ever fails.

#include "ue_wrap/daynightcycle.h"

#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ue_wrap::daynightcycle {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};
void*   g_cycleCls    = nullptr;  // daynightCycle_C UClass
int32_t g_totalTimeOff = -1;      // AdaynightCycle_C::totalTime (Alpha 0.9.0-n: 0x02B0)
int32_t g_dayOff       = -1;      // AdaynightCycle_C::Day       (0x0298)
int32_t g_timeScaleOff = -1;      // AdaynightCycle_C::TimeScale (0x02B4)
int32_t g_maxTimeOff   = -1;      // AdaynightCycle_C::MaxTime   (0x02AC) -- one day's length in totalTime units

constexpr int32_t kTotalTimeOffFallback = 0x02B0;
constexpr int32_t kDayOffFallback       = 0x0298;
constexpr int32_t kTimeScaleOffFallback = 0x02B4;
constexpr int32_t kMaxTimeOffFallback   = 0x02AC;

void* g_cycleCache = nullptr;  // cached singleton (GT-only)

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;
    void* cls = R::FindClass(L"daynightCycle_C");
    if (!cls) return false;  // not loaded yet -- caller retries

    int32_t totalOff = R::FindPropertyOffset(cls, L"totalTime");
    if (totalOff < 0) totalOff = kTotalTimeOffFallback;
    int32_t dayOff = R::FindPropertyOffset(cls, L"Day");
    if (dayOff < 0) dayOff = kDayOffFallback;
    int32_t scaleOff = R::FindPropertyOffset(cls, L"TimeScale");
    if (scaleOff < 0) scaleOff = kTimeScaleOffFallback;
    int32_t maxOff = R::FindPropertyOffset(cls, L"MaxTime");
    if (maxOff < 0) maxOff = kMaxTimeOffFallback;

    g_cycleCls     = cls;
    g_totalTimeOff = totalOff;
    g_dayOff       = dayOff;
    g_timeScaleOff = scaleOff;
    g_maxTimeOff   = maxOff;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("daynightcycle: resolved daynightCycle_C=%p totalTime@0x%04X Day@0x%04X TimeScale@0x%04X MaxTime@0x%04X",
            cls, totalOff, dayOff, scaleOff, maxOff);
    return true;
}

void* Cycle() {
    if (g_cycleCache && R::IsLive(g_cycleCache)) return g_cycleCache;  // steady-state: a pointer check
    // The cycle is a singleton that, once found, stays live -- so a re-scan only happens at startup
    // (before it streams in) or if its UObject is briefly marked unreachable mid-session. THROTTLE
    // the GUObjectArray scan to once/sec so a transient miss can never become a per-call walk (the
    // standing per-frame-FindObjectByClass ban). Game-thread-only -> the static is unguarded.
    static std::chrono::steady_clock::time_point s_lastScan{};
    const auto now = std::chrono::steady_clock::now();
    if (now - s_lastScan < std::chrono::seconds(1)) return nullptr;
    s_lastScan = now;
    if (!EnsureResolved()) return nullptr;
    g_cycleCache = R::FindObjectByClass(L"daynightCycle_C");
    return (g_cycleCache && R::IsLive(g_cycleCache)) ? g_cycleCache : nullptr;
}

bool ReadClock(float& totalTime, float& day, float& timeScale) {
    void* cyc = Cycle();
    if (!cyc || g_totalTimeOff < 0) return false;
    const char* base = reinterpret_cast<const char*>(cyc);
    totalTime = *reinterpret_cast<const float*>(base + g_totalTimeOff);
    day       = *reinterpret_cast<const float*>(base + g_dayOff);
    timeScale = *reinterpret_cast<const float*>(base + g_timeScaleOff);
    return true;
}

bool ReadMaxTime(float& maxTime) {
    void* cyc = Cycle();
    if (!cyc || g_maxTimeOff < 0) return false;
    maxTime = *reinterpret_cast<const float*>(reinterpret_cast<const char*>(cyc) + g_maxTimeOff);
    return true;
}

void ApplyClock(float totalTime, float day, float timeScale) {
    void* cyc = Cycle();
    if (!cyc || g_totalTimeOff < 0) return;
    char* base = reinterpret_cast<char*>(cyc);
    *reinterpret_cast<float*>(base + g_totalTimeOff) = totalTime;
    *reinterpret_cast<float*>(base + g_dayOff)       = day;
    *reinterpret_cast<float*>(base + g_timeScaleOff) = timeScale;
}

void WriteTimeScale(float scale) {
    void* cyc = Cycle();
    if (!cyc || g_timeScaleOff < 0) return;
    *reinterpret_cast<float*>(reinterpret_cast<char*>(cyc) + g_timeScaleOff) = scale;
}

namespace {
// dailyDelivery latch substrate (gamemode -> saveSlot -> the bool). The
// gamemode pointer is cached + liveness-revalidated (the email.cpp shape);
// the walk only re-runs after a loss, never per call.
void* g_gmCls = nullptr;
int32_t g_offGmSaveSlot = -1;
void* g_saveSlotCls = nullptr;
int32_t g_offDailyDelivery = -1;  // saveSlot_C::dailyDelivery (@0x0E40)
void* g_gm = nullptr;
int32_t g_gmIdx = -1;
}  // namespace

bool LatchDailyDelivery() {
    if (!g_gmCls) g_gmCls = R::FindClass(L"mainGamemode_C");
    if (!g_gmCls) return false;
    if (g_offGmSaveSlot < 0) g_offGmSaveSlot = R::FindPropertyOffset(g_gmCls, L"saveSlot");
    if (!g_saveSlotCls) g_saveSlotCls = R::FindClass(L"saveSlot_C");
    if (g_saveSlotCls && g_offDailyDelivery < 0)
        g_offDailyDelivery = R::FindPropertyOffset(g_saveSlotCls, L"dailyDelivery");
    if (g_offGmSaveSlot < 0 || g_offDailyDelivery < 0) return false;
    if (!g_gm || !R::IsLiveByIndex(g_gm, g_gmIdx)) {
        // Throttle the miss-path GUObjectArray walk (audit I-4: this is
        // caller-rate-driven at every 2 s clock correction; a world
        // transition would otherwise re-scan on each one -- the Cycle()
        // throttle pattern).
        static std::chrono::steady_clock::time_point s_lastScan{};
        const auto now = std::chrono::steady_clock::now();
        if (now - s_lastScan < std::chrono::seconds(2)) return false;
        s_lastScan = now;
        g_gm = nullptr;
        for (void* obj : R::FindObjectsByClass(L"mainGamemode_C")) {
            if (obj && R::IsLive(obj)) {
                g_gm = obj;
                g_gmIdx = R::InternalIndexOf(obj);
                break;
            }
        }
        if (!g_gm) return false;
    }
    void* slot = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g_gm) + g_offGmSaveSlot);
    if (!slot || !R::IsLive(slot)) return false;
    *(reinterpret_cast<uint8_t*>(slot) + g_offDailyDelivery) = 1;
    return true;
}

}  // namespace ue_wrap::daynightcycle
