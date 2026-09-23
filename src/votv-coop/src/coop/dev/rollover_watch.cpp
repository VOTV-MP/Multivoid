// coop/dev/rollover_watch.cpp -- see coop/dev/rollover_watch.h.

#include "coop/dev/rollover_watch.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/session/join_progress.h"
#include "coop/world/time_sync.h"

#include "ue_wrap/actors/sleep.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/dish.h"
#include "ue_wrap/world/daynightcycle.h"
#include "ue_wrap/world/game_mode.h"
#include "ue_wrap/world/game_rules_pane.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace coop::dev::rollover_watch {
namespace {

namespace sg  = ue_wrap::script_gate;
namespace R   = ue_wrap::reflection;
namespace DNC = ue_wrap::daynightcycle;
namespace DSH = ue_wrap::dish;
namespace SLP = ue_wrap::sleep;
namespace JP  = coop::join_progress;

using Clock = std::chrono::steady_clock;

// The twelve names. Each is declared by exactly one class in the cooked game (a census of every
// cooked Blueprint, compared case-insensitively as the engine compares names), so a name watch sees
// that class's function and nothing else. A `burst` watch prints one line per pump tick with its
// callers; the minute-rate ones fire on every instance every in-game minute and are only counted.
struct WatchDef {
    const wchar_t* name;
    const char*    declarer;
    const char*    role;
    bool           burst;
};
constexpr WatchDef kWatches[] = {
    { L"generteHashcode", "dish_C",            "roll",       true  },
    { L"setTaskNew",      "lib_C",             "roll",       true  },
    { L"Spawn Bad Sun",   "mainGamemode_C",    "roll",       true  },
    { L"func_newMinute",  "daynightCycle_C",   "pulse",      false },
    { L"func_newHour",    "daynightCycle_C",   "pulse",      true  },
    { L"newmin",          "spookpath_C",       "consumer",   false },
    { L"min",             "NewBlueprint15_C",  "consumer",   false },
    { L"newMinute",       "triggerTimer_C",    "consumer",   false },
    { L"newday",          "prop_shitlog_C",    "consumer",   true  },
    { L"spawnKrampus",    "prop_xmastree_C",   "consumer",   true  },
    { L"spawnGifts",      "prop_xmastree_C",   "consumer",   true  },
    { L"runEvent",        "trigger_eventer_C", "event walk", true  },
};
constexpr int kCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));
constexpr int kTagBase = 0x524F0000;  // 'RO', plus the watch's index
constexpr int kIdxHashcode = 0;       // its burst re-reads the digest at once

// A burst names up to this many distinct callers and counts the rest.
constexpr int kMaxCallers = 3;

// Game thread only: the gate fires its callbacks there, and Tick runs there.
struct Tally {
    uint64_t reached = 0;          // pre callbacks this session
    uint64_t ran = 0;              // post callbacks
    uint32_t burstReached = 0;     // since the last flush
    uint32_t burstRan = 0;
    void*    callerFn[kMaxCallers] = {};
    char     callerName[kMaxCallers][112] = {};
    int      callers = 0;
    bool     moreCallers = false;
    bool     firstLogged = false;  // this session's first entry printed
};
Tally g_tally[kCount];
bool  g_burstPending = false;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_registered = false;       // the twelve are in the gate's table (once per process)
bool g_registerFailed = false;   // the table refused one; said once, never retried
int  g_live = -1;                // how many are live; re-counted once a second until all are settled
bool g_liveSettled = false;      // every watch live, or the gate has no name left to resolve
Clock::time_point g_nextLiveCount{};

// Per world: the cycle actor it armed on, by identity only.
void*    g_armedCycle = nullptr;
int32_t  g_armedCycleIdx = -1;
bool     g_armed = false;
uint32_t g_saidWaiting = 0;         // the reads already named as what arming waits on, one bit each
uint64_t g_armedDigest = 0;
uint64_t g_lastDigest = 0;
Clock::time_point g_nextDigestRead{};
int32_t  g_lastOwnDayZ = INT_MIN;
int32_t  g_lastHostDayZ = -1;
float    g_dayMax = -1.f;           // the highest `day` since the last DAY line
int      g_settleTicks = -1;        // pump ticks since a DAY line, while the cycle's rebuild is awaited

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

char RoleChar() { return IsHost() ? 'H' : 'C'; }

// "Class::Function" of the frame that made the call; "<ProcessEvent>" when no Blueprint frame did
// (an engine event, a delegate, a timer, or a reflected call).
void CallerName(const sg::Call& c, char* out, size_t cap) {
    if (!c.callerFunction) { std::snprintf(out, cap, "<ProcessEvent>"); return; }
    const std::wstring fn = R::ToString(R::NameOf(c.callerFunction));
    const std::wstring cls = c.callerObject ? R::ClassNameOf(c.callerObject) : L"?";
    std::snprintf(out, cap, "%ls::%ls", cls.c_str(), fn.c_str());
}

void NoteCaller(Tally& t, const sg::Call& c) {
    for (int k = 0; k < t.callers; ++k)
        if (t.callerFn[k] == c.callerFunction) return;
    if (t.callers >= kMaxCallers) { t.moreCallers = true; return; }
    t.callerFn[t.callers] = c.callerFunction;
    CallerName(c, t.callerName[t.callers], sizeof(t.callerName[0]));
    ++t.callers;
}

sg::Verdict OnPre(const sg::Call& c) {
    const int i = c.tag - kTagBase;
    if (i < 0 || i >= kCount) return sg::Verdict::Run;
    Tally& t = g_tally[i];
    ++t.reached;
    if (!t.firstLogged) {
        t.firstLogged = true;
        char caller[112];
        CallerName(c, caller, sizeof(caller));
        void* outer = c.function ? R::OuterOf(c.function) : nullptr;
        const std::wstring decl = outer ? R::ToString(R::NameOf(outer)) : L"?";
        const std::wstring inst = c.object ? R::ClassNameOf(c.object) : L"?";
        UE_LOGI("rollover_watch: [%c] FIRST %ls (%s) -- declared by %ls, on a %ls, from %s%s", RoleChar(),
                kWatches[i].name, kWatches[i].role, decl.c_str(), inst.c_str(), caller,
                c.fromOurCode ? " (a reflected call of ours)" : "");
    }
    if (kWatches[i].burst) {
        ++t.burstReached;
        NoteCaller(t, c);
        g_burstPending = true;
    }
    return sg::Verdict::Run;
}

void OnPost(const sg::Call& c) {
    const int i = c.tag - kTagBase;
    if (i < 0 || i >= kCount) return;
    Tally& t = g_tally[i];
    ++t.ran;
    if (kWatches[i].burst) { ++t.burstRan; g_burstPending = true; }
}

// Register once per process: a name watch holds its two slots for good, so a re-register would buy
// nothing, and a table that refused one will refuse it again.
void EnsureWatches() {
    if (!g_registered && !g_registerFailed) {
        if (!sg::IsInstalled()) return;
        for (int i = 0; i < kCount; ++i) {
            if (!sg::WatchName(kWatches[i].name, kTagBase + i, &OnPre, &OnPost)) {
                g_registerFailed = true;
                UE_LOGE("rollover_watch: the gate refused the watch on %ls -- this run measures nothing",
                        kWatches[i].name);
                return;
            }
        }
        g_registered = true;
        UE_LOGI("rollover_watch: [%c] %d name watches registered; each is trusted once its name resolves",
                RoleChar(), kCount);
    }
    if (!g_registered || g_liveSettled) return;
    const auto now = Clock::now();
    if (now < g_nextLiveCount) return;
    g_nextLiveCount = now + std::chrono::seconds(1);
    sg::ResolvePendingNames();
    int live = 0;
    for (int i = 0; i < kCount; ++i)
        if (sg::NameWatchLive(kWatches[i].name, kTagBase + i)) ++live;
    const bool changed = (live != g_live);
    g_live = live;
    if (live == kCount) {
        g_liveSettled = true;
        UE_LOGI("rollover_watch: [%c] all %d name watches LIVE", RoleChar(), kCount);
    } else if (sg::PendingNameCount() == 0) {
        // Nothing is left to resolve, so a watch not live now never will be.
        g_liveSettled = true;
        UE_LOGW("rollover_watch: [%c] %d of %d name watches are DEAD (resolved into a full gate table; the gate "
                "named them) -- their counts read 0", RoleChar(), kCount - live, kCount);
    } else if (changed) {
        UE_LOGI("rollover_watch: [%c] %d of %d name watches LIVE so far", RoleChar(), live, kCount);
    }
}

// The day number and the time as the save keeps them, for the burst and digest lines.
std::string ClockBrief() {
    char buf[96];
    int32_t sh = 0, sm = 0, sz = 0;
    float total = 0, day = 0, scale = 0;
    const bool haveSaved = DNC::ReadSavedTime(sh, sm, sz);
    const bool haveClock = DNC::ReadClock(total, day, scale);
    std::snprintf(buf, sizeof(buf), "savedtime %d:%02d day %d, day=%.2f",
                  haveSaved ? sh : -1, haveSaved ? sm : -1, haveSaved ? sz : -1, haveClock ? day : -1.f);
    return buf;
}

void FlushBursts(bool& hashBurst) {
    hashBurst = false;
    if (!g_burstPending) return;
    g_burstPending = false;
    for (int i = 0; i < kCount; ++i) {
        Tally& t = g_tally[i];
        if (t.burstReached == 0 && t.burstRan == 0) continue;
        std::string callers;
        for (int k = 0; k < t.callers; ++k) {
            if (k) callers += ", ";
            callers += t.callerName[k];
        }
        if (t.moreCallers) callers += ", and others";
        UE_LOGI("rollover_watch: [%c] %ls x%u in one burst, ran %u -- %s of %s, caller %s | %s", RoleChar(),
                kWatches[i].name, t.burstReached, t.burstRan, kWatches[i].role, kWatches[i].declarer,
                callers.empty() ? "?" : callers.c_str(), ClockBrief().c_str());
        if (i == kIdxHashcode) hashBurst = true;
        t.burstReached = t.burstRan = 0;
        t.callers = 0;
        t.moreCallers = false;
        for (void*& p : t.callerFn) p = nullptr;
    }
}

// Whether the local profile holds the achievement the midnight Bad Sun roll branches on.
const char* BadsunHeld() {
    ue_wrap::game_rules_pane::LockInputs in;
    if (!ue_wrap::game_rules_pane::ReadLockInputs(in)) return "unreadable";
    for (const std::string& a : in.achievements)
        if (_stricmp(a.c_str(), "badsun") == 0) return "yes";
    return "no";
}

// Arming waits on every read below; the first time one keeps it waiting in a world, that read is
// named, so a watch that never arms says why instead of staying silent.
void Waiting(uint32_t bit, const char* what) {
    if (g_saidWaiting & bit) return;
    g_saidWaiting |= bit;
    UE_LOGI("rollover_watch: [%c] not armed yet -- %s; arming when it is", RoleChar(), what);
}

void TryArm() {
    float total = 0, day = 0, scale = 0, maxTime = 0;
    int32_t th = 0, tm = 0, tz = 0, sh = 0, sm = 0, sz = 0;
    if (!DNC::ReadClock(total, day, scale)) { Waiting(1u << 0, "the clock does not read"); return; }
    if (!DNC::ReadMaxTime(maxTime)) { Waiting(1u << 1, "the day length does not read"); return; }
    if (!DNC::ReadTimeZ(th, tm, tz)) { Waiting(1u << 2, "the cycle's timeZ does not read"); return; }
    if (!DNC::ReadSavedTime(sh, sm, sz)) { Waiting(1u << 3, "the save's savedtime does not read"); return; }
    DSH::HashDigest dg{};
    if (!DSH::ReadHashDigest(dg)) { Waiting(1u << 4, "the dish hash codes do not read"); return; }
    if (dg.dishes <= 0) { Waiting(1u << 5, "the clock reads but gamemode.dishs is empty"); return; }
    DNC::Rates rt{};
    const bool haveRates = DNC::ReadRates(rt);
    const int mode = ue_wrap::game_mode::ReadLocal();
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    const int32_t hostDay = IsHost() ? sz : coop::time_sync::LastHostDayZ();
    char rates[160];
    if (haveRates)
        std::snprintf(rates, sizeof(rates), "realtime=%d diff_mult=%.3f settingMultiplayer=%.3f "
                      "sleepingTimeDilation=%.3f", rt.realtime ? 1 : 0, rt.diffMult, rt.settingMultiplayer,
                      rt.sleepingTimeDilation);
    else
        std::snprintf(rates, sizeof(rates), "rates UNRESOLVED");
    UE_LOGI("rollover_watch: [%c] ARMED -- %s maxTime=%.1f timescale=%.3f | mode=%d (%s) savedtime %d:%02d "
            "day %d, timeZ(diag) %d:%02d day %d, day=%.2f, host day %d | digest %016llx filled %d/%d | "
            "calendar %04u-%02u-%02u, badsun %s | gate %s, watches live %d/%d",
            RoleChar(), rates, maxTime, scale, mode, ue_wrap::game_mode::NameOrOrdinal(mode).c_str(), sh, sm, sz,
            th, tm, tz, day, hostDay, static_cast<unsigned long long>(dg.digest), dg.filled, dg.dishes,
            st.wYear, st.wMonth, st.wDay, BadsunHeld(), sg::IsEnabled() ? "on" : "OFF",
            g_live < 0 ? 0 : g_live, kCount);
    g_armed = true;
    g_armedDigest = g_lastDigest = dg.digest;
    g_nextDigestRead = Clock::now() + std::chrono::seconds(1);
    g_lastOwnDayZ = sz;
    g_lastHostDayZ = IsHost() ? -1 : coop::time_sync::LastHostDayZ();
    g_dayMax = day;
}

// Re-read at every generteHashcode burst and once a second; printed on change.
void CheckDigest(bool now) {
    const auto t = Clock::now();
    if (!now && t < g_nextDigestRead) return;
    g_nextDigestRead = t + std::chrono::seconds(1);
    DSH::HashDigest dg{};
    if (!DSH::ReadHashDigest(dg) || dg.digest == g_lastDigest) return;
    UE_LOGI("rollover_watch: [%c] DIGEST %016llx -> %016llx filled %d/%d (%s) | %s", RoleChar(),
            static_cast<unsigned long long>(g_lastDigest), static_cast<unsigned long long>(dg.digest), dg.filled,
            dg.dishes, now ? "after a generteHashcode burst" : "the 1 Hz re-read", ClockBrief().c_str());
    g_lastDigest = dg.digest;
}

void PrintDay(const char* trigger, float day, float scale) {
    int32_t sh = 0, sm = 0, sz = 0, th = 0, tm = 0, tz = 0;
    DNC::ReadSavedTime(sh, sm, sz);
    DNC::ReadTimeZ(th, tm, tz);
    float maxTime = 0;
    DNC::ReadMaxTime(maxTime);
    SLP::EnsureResolved();
    DSH::HashDigest dg{};
    const bool haveDigest = DSH::ReadHashDigest(dg);
    const bool host = IsHost();
    // Whether this peer's clock saw the day's last game minute before the new day: the premise an
    // absence of roll entries is read against. The cycle adds and wraps inside one tick, so the highest
    // `day` sampled between ticks is always below maxTime and says nothing by itself.
    const bool lastMinute = maxTime > 0.f && g_dayMax >= maxTime - maxTime / 1440.f;
    UE_LOGI("rollover_watch: [%c] DAY (%s) -- host day %d | savedtime %d:%02d day %d, timeZ(diag) %d:%02d day %d | "
            "day=%.2f dayMax=%.2f (the last minute %s) maxTime=%.1f timescale=%.3f dilation=%.2f isSleep=%d | "
            "phase %s, gate %s | digest %016llx filled %d/%d, %s since ARMED",
            RoleChar(), trigger, host ? sz : coop::time_sync::LastHostDayZ(), sh, sm, sz, th, tm, tz, day, g_dayMax,
            lastMinute ? "reached" : "NOT reached", maxTime, scale, SLP::GetGlobalTimeDilation(),
            SLP::IsSleeping() ? 1 : 0,
            host ? "host" : JP::PhaseName(JP::CurrentPhase()), sg::IsEnabled() ? "on" : "OFF",
            static_cast<unsigned long long>(haveDigest ? dg.digest : 0), haveDigest ? dg.filled : -1,
            haveDigest ? dg.dishes : -1,
            !haveDigest ? "unreadable" : (dg.digest != g_armedDigest ? "MOVED" : "unchanged"));
    char totals[900];
    int n = std::snprintf(totals, sizeof(totals), "rollover_watch: [%c] DAY totals, reached/ran --", RoleChar());
    for (int i = 0; i < kCount && n > 0 && n < static_cast<int>(sizeof(totals)); ++i)
        n += std::snprintf(totals + n, sizeof(totals) - n, " %ls=%llu/%llu", kWatches[i].name,
                           static_cast<unsigned long long>(g_tally[i].reached),
                           static_cast<unsigned long long>(g_tally[i].ran));
    UE_LOGI("%s", totals);
}

// A DAY line on the host when its own day number moves; on a client when the host's day number (the
// last clock sample's) moves, and again when its own does: our write of the host's, or a roll of its
// own. The cycle's rebuilt named clock is then read back.
void CheckDay(float day, float scale) {
    int32_t sh = 0, sm = 0, sz = 0;
    if (!DNC::ReadSavedTime(sh, sm, sz)) return;
    const bool own = (sz != g_lastOwnDayZ);
    bool hostMoved = false;
    int32_t hostDay = -1;
    if (!IsHost()) {
        hostDay = coop::time_sync::LastHostDayZ();
        if (hostDay >= 0 && g_lastHostDayZ < 0) g_lastHostDayZ = hostDay;  // the first correction is a baseline
        hostMoved = (hostDay >= 0 && hostDay != g_lastHostDayZ);
    }
    if (!own && !hostMoved) return;
    PrintDay(own && hostMoved ? "the host's day number and its own" : own ? "its own day number"
                                                                         : "the host's day number",
             day, scale);
    g_lastOwnDayZ = sz;
    if (hostMoved) g_lastHostDayZ = hostDay;
    g_dayMax = day;
    g_settleTicks = 0;
}

// After a DAY line, the cycle's own rebuild of the named clock from `day` and the day number: read
// once its day matches, or after 30 pump ticks, which the line then says. The pump is not in step with
// the cycle's tick, so a fixed one-tick wait could read the value from before the rebuild.
void CheckSettled() {
    int32_t sh = 0, sm = 0, sz = 0, th = 0, tm = 0, tz = 0;
    if (!DNC::ReadSavedTime(sh, sm, sz) || !DNC::ReadTimeZ(th, tm, tz)) return;
    ++g_settleTicks;
    if (tz != sz && g_settleTicks < 30) return;
    UE_LOGI("rollover_watch: [%c] DAY settled after %d pump ticks -- timeZ %d:%02d day %d, savedtime %d:%02d day "
            "%d, host day %d%s", RoleChar(), g_settleTicks, th, tm, tz, sh, sm, sz,
            IsHost() ? sz : coop::time_sync::LastHostDayZ(), tz == sz ? "" : " (the rebuild never caught up)");
    g_settleTicks = -1;
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::rollover_watch);
    return s;
}

void Install(coop::net::Session* session) {
    if (!IsEnabled()) return;
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!IsEnabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    EnsureWatches();
    bool hashBurst = false;
    FlushBursts(hashBurst);

    // A new world is a new cycle actor; arm on it. Cycle hands out a live actor only, so reading
    // its slot index here is safe; the pointer and the index are kept as identity and never read.
    void* cyc = DNC::Cycle();
    if (!cyc) return;
    const int32_t idx = R::InternalIndexOf(cyc);
    if (cyc != g_armedCycle || idx != g_armedCycleIdx) {
        g_armedCycle = cyc;
        g_armedCycleIdx = idx;
        g_armed = false;
        g_saidWaiting = 0;
    }
    if (!g_armed) {
        TryArm();
        return;
    }
    float total = 0, day = 0, scale = 0;
    if (!DNC::ReadClock(total, day, scale)) return;
    if (day > g_dayMax) g_dayMax = day;
    CheckDigest(hashBurst);
    if (g_settleTicks >= 0) CheckSettled();
    CheckDay(day, scale);
}

uint64_t RanCount(const wchar_t* name) {
    for (int i = 0; i < kCount; ++i)
        if (std::wcscmp(kWatches[i].name, name) == 0) return g_tally[i].ran;
    return 0;
}

void OnDisconnect() {
    for (Tally& t : g_tally) t = Tally{};
    g_burstPending = false;
    g_armedCycle = nullptr;
    g_armedCycleIdx = -1;
    g_armed = false;
    g_saidWaiting = 0;
    g_armedDigest = g_lastDigest = 0;
    g_lastOwnDayZ = INT_MIN;
    g_lastHostDayZ = -1;
    g_dayMax = -1.f;
    g_settleTicks = -1;
}

}  // namespace coop::dev::rollover_watch
