// ue_wrap/world/daynightcycle.cpp -- see ue_wrap/world/daynightcycle.h. Engine access for VOTV's
// world clock (AdaynightCycle_C).
//
// Offsets are resolved from the live class via reflection (FindPropertyOffset); the known
// Alpha 0.9.0-n offsets are a logged fallback if the reflected walk ever fails.

#include "ue_wrap/world/daynightcycle.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/reflection.h"

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
int32_t g_timeZOff     = -1;      // AdaynightCycle_C::timeZ     (0x02D0) -- FIntVector (hour, minute, DAY)
// The rate inputs, read by instruments only, so no fallback: an unresolved one fails the read.
int32_t g_realtimeOff  = -1;      // AdaynightCycle_C::realtime (bool)
int32_t g_diffMultOff  = -1;      // AdaynightCycle_C::diff_mult
int32_t g_settingMpOff = -1;      // AdaynightCycle_C::settingMultiplayer (the rules' day speed)
int32_t g_sleepDilOff  = -1;      // AdaynightCycle_C::sleepingTimeDilation

constexpr int32_t kTotalTimeOffFallback = 0x02B0;
constexpr int32_t kDayOffFallback       = 0x0298;
constexpr int32_t kTimeScaleOffFallback = 0x02B4;
constexpr int32_t kMaxTimeOffFallback   = 0x02AC;
constexpr int32_t kTimeZOffFallback     = 0x02D0;

// Cached singleton (GT-only). CachedObjRef subsumes the hand-rolled
// {ptr, g_cycleCacheIdx} pair (RULE 2; islive-zeroav D1) -- IsLiveByIndex (serial
                                 // slot-compare) rejects a RECYCLED slot that plain
                                 // IsLive accepts; raw float writes through a recycled
                                 // pointer corrupt the foreign occupant, which is
                                 // reachable from the quit-to-menu teardown)
ue_wrap::CachedObjRef g_cycleCache;
void* g_tickFn = nullptr;         // daynightCycle_C::ReceiveTick
int32_t g_cycleGmOff = -1;        // AdaynightCycle_C::gamemode

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
    int32_t timeZOff = R::FindPropertyOffset(cls, L"timeZ");
    if (timeZOff < 0) timeZOff = kTimeZOffFallback;

    g_cycleCls     = cls;
    g_totalTimeOff = totalOff;
    g_dayOff       = dayOff;
    g_timeScaleOff = scaleOff;
    g_maxTimeOff   = maxOff;
    g_timeZOff     = timeZOff;
    g_realtimeOff  = R::FindPropertyOffset(cls, L"realtime");
    g_diffMultOff  = R::FindPropertyOffset(cls, L"diff_mult");
    g_settingMpOff = R::FindPropertyOffset(cls, L"settingMultiplayer");
    g_sleepDilOff  = R::FindPropertyOffset(cls, L"sleepingTimeDilation");
    g_tickFn       = R::FindFunction(cls, L"ReceiveTick");
    g_cycleGmOff   = R::FindPropertyOffset(cls, L"gamemode");
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("daynightcycle: resolved daynightCycle_C=%p totalTime@0x%04X Day@0x%04X TimeScale@0x%04X MaxTime@0x%04X timeZ@0x%04X",
            cls, totalOff, dayOff, scaleOff, maxOff, timeZOff);
    return true;
}

void* Cycle() {
    if (g_cycleCache.Alive())
        return g_cycleCache.Raw();  // steady-state: a slot compare
    // The cycle is a singleton that, once found, stays live -- so a re-scan only happens at startup
    // (before it streams in) or if its UObject is briefly marked unreachable mid-session. THROTTLE
    // the GUObjectArray scan to once/sec so a transient miss can never become a per-call walk (the
    // standing per-frame-FindObjectByClass ban). Game-thread-only -> the static is unguarded.
    static std::chrono::steady_clock::time_point s_lastScan{};
    const auto now = std::chrono::steady_clock::now();
    if (now - s_lastScan < std::chrono::seconds(1)) return nullptr;
    s_lastScan = now;
    if (!EnsureResolved()) return nullptr;
    g_cycleCache.Set(R::FindObjectByClass(L"daynightCycle_C"));
    return g_cycleCache.Raw();  // fresh from the walk (null on miss)
}

void* TickFunction() { return g_resolved.load(std::memory_order_acquire) ? g_tickFn : nullptr; }

void NoteCycle(void* cycle) {
    // A new world's cycle may sit at the old one's address; the cached slot serial tells them apart.
    if (cycle && (cycle != g_cycleCache.Raw() || !g_cycleCache.Alive())) g_cycleCache.Set(cycle);
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

void ApplyClock(float totalTime, float day) {
    void* cyc = Cycle();
    if (!cyc || g_totalTimeOff < 0) return;
    char* base = reinterpret_cast<char*>(cyc);
    *reinterpret_cast<float*>(base + g_totalTimeOff) = totalTime;
    *reinterpret_cast<float*>(base + g_dayOff)       = day;
}

void WriteTimeScale(float scale) {
    void* cyc = Cycle();
    if (!cyc || g_timeScaleOff < 0) return;
    *reinterpret_cast<float*>(reinterpret_cast<char*>(cyc) + g_timeScaleOff) = scale;
}

bool ReadTimeZ(int32_t& hour, int32_t& minute, int32_t& day) {
    void* cyc = Cycle();
    if (!cyc || g_timeZOff < 0) return false;
    const int32_t* v = reinterpret_cast<const int32_t*>(reinterpret_cast<const char*>(cyc) + g_timeZOff);
    hour = v[0];
    minute = v[1];
    day = v[2];
    return true;
}

bool ReadRates(Rates& out) {
    void* cyc = Cycle();
    if (!cyc || g_realtimeOff < 0 || g_diffMultOff < 0 || g_settingMpOff < 0 || g_sleepDilOff < 0)
        return false;
    const char* base = reinterpret_cast<const char*>(cyc);
    out.realtime             = *reinterpret_cast<const uint8_t*>(base + g_realtimeOff) != 0;
    out.diffMult             = *reinterpret_cast<const float*>(base + g_diffMultOff);
    out.settingMultiplayer   = *reinterpret_cast<const float*>(base + g_settingMpOff);
    out.sleepingTimeDilation = *reinterpret_cast<const float*>(base + g_sleepDilOff);
    return true;
}

namespace {
// The saveSlot substrate (gamemode -> saveSlot), shared by the delivery latch and the saved clock.
// The gamemode pointer is cached + liveness-revalidated (the email.cpp shape); the walk only
// re-runs after a loss, never per call.
void* g_gmCls = nullptr;
int32_t g_offGmSaveSlot = -1;
void* g_saveSlotCls = nullptr;
int32_t g_offDailyDelivery = -1;  // saveSlot_C::dailyDelivery
int32_t g_offSavedTime = -1;      // saveSlot_C::savedtime (FIntVector)
// Held world-stamped: a dying world's gamemode keeps its slot until the purge, tens of seconds
// after the world changed, and its saveSlot is not the running world's.
CachedObjRef g_gm;

// Resolve the two classes and the gamemode's saveSlot member. False while either class is unloaded.
bool ResolveSaveSlotSurface() {
    if (!g_gmCls) g_gmCls = R::FindClass(L"mainGamemode_C");
    if (!g_gmCls) return false;
    if (g_offGmSaveSlot < 0) g_offGmSaveSlot = R::FindPropertyOffset(g_gmCls, L"saveSlot");
    if (!g_saveSlotCls) g_saveSlotCls = R::FindClass(L"saveSlot_C");
    return g_offGmSaveSlot >= 0 && g_saveSlotCls != nullptr;
}

// The live saveSlot, or null. Call after ResolveSaveSlotSurface answered true.
void* LiveSaveSlot() {
    if (!g_gm.Alive()) {
        // Throttle the miss-path GUObjectArray walk: the callers are caller-rate-driven -- the
        // latch and the day number once per streamed clock sample, the savedtime read by the dev
        // rollover watch every tick -- so a world transition would otherwise re-scan on every call. A world change
        // lifts the throttle: it is there for a gamemode that is missing, not one a new world brought.
        static std::chrono::steady_clock::time_point s_lastScan{};
        static uint32_t s_scanGen = 0;
        const auto now = std::chrono::steady_clock::now();
        const uint32_t gen = world_identity::Generation();
        if (gen == s_scanGen && now - s_lastScan < std::chrono::seconds(2)) return nullptr;
        s_lastScan = now;
        s_scanGen = gen;
        g_gm.Reset();
        for (void* obj : R::FindObjectsByClass(L"mainGamemode_C")) {
            if (obj && R::IsLive(obj)) {
                g_gm.Set(obj);
                if (g_gm.Alive()) break;
                g_gm.Reset();
            }
        }
        if (!g_gm.Alive()) return nullptr;
    }
    void* slot = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g_gm.Raw()) + g_offGmSaveSlot);
    return (slot && R::IsLive(slot)) ? slot : nullptr;
}
}  // namespace

void* SaveSlotOfCycle(void* cycle) {
    if (!cycle || g_cycleGmOff < 0) return nullptr;
    void* gm = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cycle) + g_cycleGmOff);
    if (!gm || !R::IsLive(gm)) return nullptr;
    // From the live gamemode's own class: a class lookup by name walks the object array on a miss.
    if (g_offGmSaveSlot < 0) g_offGmSaveSlot = R::FindPropertyOffset(R::ClassOf(gm), L"saveSlot");
    if (g_offGmSaveSlot < 0) return nullptr;
    void* slot = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offGmSaveSlot);
    return (slot && R::IsLive(slot)) ? slot : nullptr;
}

bool LatchDailyDelivery() {
    if (!ResolveSaveSlotSurface()) return false;
    if (g_offDailyDelivery < 0)
        g_offDailyDelivery = R::FindPropertyOffset(g_saveSlotCls, L"dailyDelivery");
    if (g_offDailyDelivery < 0) return false;
    void* slot = LiveSaveSlot();
    if (!slot) return false;
    *(reinterpret_cast<uint8_t*>(slot) + g_offDailyDelivery) = 1;
    return true;
}

namespace {
// The live saveSlot's savedtime triple, or null. The offset is resolved on the first call.
int32_t* SavedTimeOf() {
    if (!ResolveSaveSlotSurface()) return nullptr;
    if (g_offSavedTime < 0) g_offSavedTime = R::FindPropertyOffset(g_saveSlotCls, L"savedtime");
    if (g_offSavedTime < 0) return nullptr;
    void* slot = LiveSaveSlot();
    if (!slot) return nullptr;
    return reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(slot) + g_offSavedTime);
}
}  // namespace

bool ReadSavedTime(int32_t& hour, int32_t& minute, int32_t& day) {
    const int32_t* v = SavedTimeOf();
    if (!v) return false;
    hour = v[0];
    minute = v[1];
    day = v[2];
    return true;
}

bool WriteSavedDay(int32_t day) {
    int32_t* v = SavedTimeOf();
    if (!v) return false;
    v[2] = day;
    return true;
}

}  // namespace ue_wrap::daynightcycle
