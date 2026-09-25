// ue_wrap/world/daynightcycle.cpp -- see ue_wrap/world/daynightcycle.h. Engine access for VOTV's
// world clock (AdaynightCycle_C).
//
// Offsets are resolved from the live class via reflection (FindPropertyOffset); the known
// Alpha 0.9.0-n offsets are a logged fallback if the reflected walk ever fails.

#include "ue_wrap/world/daynightcycle.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"

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

void* g_tickFn = nullptr;         // daynightCycle_C::ReceiveTick
int32_t g_cycleGmOff = -1;        // AdaynightCycle_C::gamemode

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;
    // Until the class loads every caller retries -- the install fanout every pump tick -- and a
    // class lookup that misses walks the whole object array: one attempt a second.
    static std::chrono::steady_clock::time_point s_nextTry{};
    const auto now = std::chrono::steady_clock::now();
    if (now < s_nextTry) return false;
    s_nextTry = now + std::chrono::seconds(1);
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
    if (g_cycleGmOff < 0)
        UE_LOGW("daynightcycle: daynightCycle_C has no gamemode -- a ticked cycle's save slot and menu flag "
                "are out of reach");
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("daynightcycle: resolved daynightCycle_C=%p totalTime@0x%04X Day@0x%04X TimeScale@0x%04X MaxTime@0x%04X timeZ@0x%04X",
            cls, totalOff, dayOff, scaleOff, maxOff, timeZOff);
    return true;
}

void* Cycle() {
    if (!EnsureResolved()) return nullptr;
    return world_singleton::Find(L"daynightCycle_C");
}

void* TickFunction() { return g_resolved.load(std::memory_order_acquire) ? g_tickFn : nullptr; }

bool ReadClockOf(void* cycle, float& totalTime, float& day, float& timeScale) {
    if (!cycle || g_totalTimeOff < 0) return false;
    const char* base = reinterpret_cast<const char*>(cycle);
    totalTime = *reinterpret_cast<const float*>(base + g_totalTimeOff);
    day       = *reinterpret_cast<const float*>(base + g_dayOff);
    timeScale = *reinterpret_cast<const float*>(base + g_timeScaleOff);
    return true;
}

bool ReadClock(float& totalTime, float& day, float& timeScale) {
    return ReadClockOf(Cycle(), totalTime, day, timeScale);
}

bool ReadMaxTimeOf(void* cycle, float& maxTime) {
    if (!cycle || g_maxTimeOff < 0) return false;
    maxTime = *reinterpret_cast<const float*>(reinterpret_cast<const char*>(cycle) + g_maxTimeOff);
    return true;
}

bool ReadMaxTime(float& maxTime) { return ReadMaxTimeOf(Cycle(), maxTime); }

void ApplyClockOf(void* cycle, float totalTime, float day) {
    if (!cycle || g_totalTimeOff < 0) return;
    char* base = reinterpret_cast<char*>(cycle);
    *reinterpret_cast<float*>(base + g_totalTimeOff) = totalTime;
    *reinterpret_cast<float*>(base + g_dayOff)       = day;
}

void ApplyClock(float totalTime, float day) { ApplyClockOf(Cycle(), totalTime, day); }

void WriteTimeScaleOf(void* cycle, float scale) {
    if (!cycle || g_timeScaleOff < 0) return;
    *reinterpret_cast<float*>(reinterpret_cast<char*>(cycle) + g_timeScaleOff) = scale;
}

void WriteTimeScale(float scale) { WriteTimeScaleOf(Cycle(), scale); }

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
// A member offset read from a live object's own class once, and latched either way: a class that
// lacks the member -- a game update renamed it -- lacks it for the process, so the lookup, a walk
// of the class's fields with a name rendered for each, never repeats, and the miss is said once.
struct LatchedMember {
    const wchar_t* cls;
    const wchar_t* name;
    const char* without;  // what cannot be done without it
    int32_t off = -1;
    bool missing = false;

    int32_t Of(void* obj) {
        if (off >= 0 || missing || !obj) return off;
        off = R::FindPropertyOffset(R::ClassOf(obj), name);
        if (off < 0) {
            missing = true;
            UE_LOGW("daynightcycle: %ls has no %ls -- %s", cls, name, without);
        }
        return off;
    }
};

// The saveSlot substrate (gamemode -> saveSlot). The running world's gamemode, for the callers with no
// cycle in hand (the host's clock sample, the dev instruments), is the world singleton's.
LatchedMember g_gmSaveSlot{L"mainGamemode_C", L"saveSlot", "the save slot's clock fields are out of reach"};
LatchedMember g_slotDailyDelivery{L"saveSlot_C", L"dailyDelivery", "the 6 am order latch cannot be set"};
LatchedMember g_slotSavedTime{L"saveSlot_C", L"savedtime", "the day number can be neither read nor written"};
LatchedMember g_slotMusics{L"saveSlot_C", L"musics", "the day's music flags cannot be set again"};
LatchedMember g_cycleSleepless{L"daynightCycle_C", L"sleeplessDays", "the midnights since load cannot be counted"};

// UE4.27 TArray<bool>: its data, then its count and capacity; one byte an element.
struct BoolArray {
    uint8_t* data;
    int32_t num;
    int32_t max;
};
BoolArray* MusicsIn(void* saveSlot) {
    const int32_t off = g_slotMusics.Of(saveSlot);
    if (!saveSlot || off < 0) return nullptr;
    return reinterpret_cast<BoolArray*>(reinterpret_cast<uint8_t*>(saveSlot) + off);
}
// The running world's live saveSlot, for a caller with no cycle in hand, or null.
void* LiveSaveSlot() {
    void* gm = world_singleton::Gamemode();   // world-stamped: never a dying world's
    if (!gm) return nullptr;
    const int32_t off = g_gmSaveSlot.Of(gm);
    if (off < 0) return nullptr;
    void* slot = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + off);
    return (slot && R::IsLive(slot)) ? slot : nullptr;
}
}  // namespace

void* SaveSlotOfCycle(void* cycle) {
    if (!cycle || g_cycleGmOff < 0) return nullptr;
    void* gm = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cycle) + g_cycleGmOff);
    if (!gm || !R::IsLive(gm)) return nullptr;
    const int32_t off = g_gmSaveSlot.Of(gm);
    if (off < 0) return nullptr;
    void* slot = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + off);
    return (slot && R::IsLive(slot)) ? slot : nullptr;
}

bool IsMenuCycle(void* cycle) {
    static int32_t s_off = -1;  // mainGamemode_C::isMainMenu, its byte and bit
    static uint8_t s_mask = 0;
    static bool s_missing = false;
    if (!cycle || g_cycleGmOff < 0 || s_missing) return false;
    void* gm = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cycle) + g_cycleGmOff);
    if (!gm || !R::IsLive(gm)) return false;
    if (s_off < 0 && !R::FindBoolProperty(R::ClassOf(gm), L"isMainMenu", s_off, s_mask)) {
        s_missing = true;
        UE_LOGW("daynightcycle: mainGamemode_C has no isMainMenu -- the menu's cycle reads as a world's");
        return false;
    }
    return (*(reinterpret_cast<const uint8_t*>(gm) + s_off) & s_mask) != 0;
}

bool ReadSleeplessDaysOf(void* cycle, int32_t& out) {
    const int32_t off = g_cycleSleepless.Of(cycle);
    if (!cycle || off < 0) return false;
    out = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(cycle) + off);
    return true;
}

bool WriteSleeplessDaysOf(void* cycle, int32_t v) {
    const int32_t off = g_cycleSleepless.Of(cycle);
    if (!cycle || off < 0) return false;
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(cycle) + off) = v;
    return true;
}

bool ReadMusicsOf(void* saveSlot, int32_t& set, int32_t& count) {
    const BoolArray* a = MusicsIn(saveSlot);
    if (!a || a->num < 0 || (a->num > 0 && !a->data)) return false;
    set = 0;
    for (int32_t i = 0; i < a->num; ++i) set += a->data[i] ? 1 : 0;
    count = a->num;
    return true;
}

bool WriteAllMusicsOf(void* saveSlot, bool set) {
    BoolArray* a = MusicsIn(saveSlot);
    if (!a || a->num < 0 || (a->num > 0 && !a->data)) return false;
    for (int32_t i = 0; i < a->num; ++i) a->data[i] = set ? 1 : 0;
    return true;
}

bool LatchDailyDeliveryOf(void* saveSlot) {
    const int32_t off = g_slotDailyDelivery.Of(saveSlot);
    if (!saveSlot || off < 0) return false;
    *(reinterpret_cast<uint8_t*>(saveSlot) + off) = 1;
    return true;
}

namespace {
// A save slot's savedtime triple, or null.
int32_t* SavedTimeIn(void* saveSlot) {
    const int32_t off = g_slotSavedTime.Of(saveSlot);
    if (!saveSlot || off < 0) return nullptr;
    return reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(saveSlot) + off);
}
}  // namespace

bool ReadSavedTimeOf(void* saveSlot, int32_t& hour, int32_t& minute, int32_t& day) {
    const int32_t* v = SavedTimeIn(saveSlot);
    if (!v) return false;
    hour = v[0];
    minute = v[1];
    day = v[2];
    return true;
}

bool ReadSavedTime(int32_t& hour, int32_t& minute, int32_t& day) {
    return ReadSavedTimeOf(LiveSaveSlot(), hour, minute, day);
}

bool WriteSavedDayOf(void* saveSlot, int32_t day) {
    int32_t* v = SavedTimeIn(saveSlot);
    if (!v) return false;
    v[2] = day;
    return true;
}

bool WriteSavedDay(int32_t day) { return WriteSavedDayOf(LiveSaveSlot(), day); }

}  // namespace ue_wrap::daynightcycle
