// coop/world/time_sync.cpp -- see coop/world/time_sync.h. Host-authoritative world-clock sync.

#include "coop/world/time_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/world/daynightcycle.h"

#include <atomic>
#include <chrono>
#include <cmath>

namespace coop::time_sync {
namespace {

namespace DNC = ue_wrap::daynightcycle;
namespace GT  = ue_wrap::game_thread;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};
// The host's day number as the last applied sample carried it; -1 before the first.
std::atomic<int32_t> g_lastHostDayZ{-1};
bool g_tickObserved = false;  // the pre-observer on the cycle's tick is registered (once per process)

// HOST: when a sample is due. One is sent when the clock has moved half a game minute since the last
// one sent, so a sample never carries the clock across more than one minute boundary even with one
// datagram lost, and at the latest after the resting interval. Measured on the clock itself (the
// day number times maxTime, plus `day`), whatever moved it: the sleep's time dilation, the difficulty,
// the day-length rule, a rewind or a set clock, which is sent at once.
constexpr double kGameMinutesPerDay = 1440.0;
constexpr auto   kRestInterval = std::chrono::milliseconds(500);
double g_sentAbs = -1.0;
Clock::time_point g_sentAt{};

// CLIENT, game thread. The cycle whose time scale we hold -- each world brings its own, and the one
// held last is handed back on disconnect -- the last applied absolute clock, the backward run in
// progress against it, and the counts behind the rate-limited lines.
ue_wrap::CachedObjRef g_heldCycle;
bool     g_haveLastAbs = false;
double   g_lastAbs = 0.0;
uint32_t g_backRun = 0;         // consecutive samples that stepped back
double   g_backRunUnits = 0.0;  // and how far, in all
uint32_t g_scaleOverrides = 0;
uint32_t g_malformed = 0;
uint32_t g_appliedSince = 0;  // samples since the last convergence line, which reports them every 10 s
Clock::time_point g_nextStreamLine{};

// Receive-side format check: a NaN or absurd clock written raw into the cycle reaches the sun and
// moon rotation (a black sky, a rotator assert). The accumulators are game seconds, thousands per
// day, and `day` goes below zero while the game's rewind runs the clock backward; the day number
// counts days.
bool IsWellFormed(const coop::net::TimeSyncPayload& p) {
    return std::isfinite(p.totalTime) && std::isfinite(p.day) && std::fabs(p.totalTime) <= 1.0e7f &&
           std::fabs(p.day) <= 1.0e7f && p.dayZ >= 0 && p.dayZ <= 1000000;
}

// HOST: the sample, its absolute clock and the day length. False until the cycle and the save slot
// are streamed in, so garbage is never sent.
bool MakePayload(coop::net::TimeSyncPayload& out, double& absOut, float& maxTimeOut) {
    float t = 0, d = 0, s = 0, maxT = 0;
    if (!DNC::ReadClock(t, d, s) || !DNC::ReadMaxTime(maxT) || maxT <= 0.f) return false;
    int32_t h = 0, m = 0, dz = 0;
    if (!DNC::ReadSavedTime(h, m, dz)) return false;
    out.totalTime = t;
    out.day = d;
    out.dayZ = dz;
    absOut = static_cast<double>(dz) * maxT + d;
    maxTimeOut = maxT;
    return true;
}

// CLIENT: park the ticked cycle's clock. Its first tick under the session parks it -- a new world's
// cycle starts at the game's own rate -- and after that a game writer on this machine, the cheat
// menu or the purple wisp's rewind, is overwritten before the tick reads it, and named.
void HoldTimeScale(void* cycle) {
    float t = 0, d = 0, s = 0;
    if (!DNC::ReadClockOf(cycle, t, d, s)) return;
    if (!g_heldCycle.Is(cycle)) {
        g_heldCycle.Set(cycle);
        if (s != 0.f) DNC::WriteTimeScaleOf(cycle, 0.f);
        UE_LOGI("time_sync: client clock parked (time scale %.2f -> 0; the host's samples move it)", s);
        return;
    }
    if (s == 0.f) return;
    DNC::WriteTimeScaleOf(cycle, 0.f);
    const uint32_t n = ++g_scaleOverrides;
    if (n <= 5 || (n % 50) == 0)
        UE_LOGW("time_sync: the client's clock rate was set to %.2f on this machine -- held at 0, the host owns "
                "the clock (#%u)", s, n);
}

// CLIENT: say a backward run once, when it ends. The host's rewind runs its clock back for an hour,
// so every sample inside it steps back; a clock set back steps back once.
void EndBackRun() {
    if (g_backRun == 0) return;
    UE_LOGI("time_sync: the host's clock went back %.2f units over %u sample(s) -- its rewind or a set clock",
            g_backRunUnits, g_backRun);
    g_backRun = 0;
    g_backRunUnits = 0.0;
}

// CLIENT: write one host sample into the parked cycle and its own save slot. The day number is
// written only when it moved, and never the hour and minute: those are the client's settime's, so
// its pulses fire as the host's samples cross each minute.
void ApplyClockSnapshot(void* cycle, const coop::net::TimeSyncPayload& p, float maxT) {
    DNC::ApplyClockOf(cycle, p.totalTime, p.day);
    void* slot = DNC::SaveSlotOfCycle(cycle);
    int32_t sh = 0, sm = 0, sz = 0;
    if (DNC::ReadSavedTimeOf(slot, sh, sm, sz) && sz != p.dayZ && DNC::WriteSavedDayOf(slot, p.dayZ))
        UE_LOGI("time_sync: day number %d -> %d, the host's", sz, p.dayZ);
    const double abs = static_cast<double>(p.dayZ) * maxT + p.day;
    if (g_haveLastAbs && abs < g_lastAbs) {
        ++g_backRun;
        g_backRunUnits += g_lastAbs - abs;
    } else {
        EndBackRun();
    }
    g_lastAbs = abs;
    g_haveLastAbs = true;
    g_lastHostDayZ.store(p.dayZ, std::memory_order_release);
    // The 6 am order latch: func_newHour still runs on the client's pulse, and the game's own reset
    // of the flag is part of the midnight the client no longer rolls; a save load could reset it.
    DNC::LatchDailyDeliveryOf(slot);
}

// CLIENT: the cycle is about to tick. Park it and write the newest host sample into it, so the tick
// reads the host's clock and a zero rate every time, the first tick of a new world included.
void OnCycleTickPre(void* self, void* /*function*/, void* /*params*/) {
    if (!GT::IsGameThread() || !HoldsCycle(self)) return;
    auto* s = g_session.load(std::memory_order_acquire);  // set, since HoldsCycle answered yes
    HoldTimeScale(self);
    coop::net::TimeSyncPayload p{};
    bool isNew = false;
    if (!s->TryGetHostClock(p, &isNew) || !isNew) return;
    // A `day` past the day's end would run this client's own midnight at the tick -- the game's roll
    // test is `day > maxTime` -- and the host wraps inside its own tick, so no sample it makes has one.
    float maxT = 0.f;
    if (!IsWellFormed(p) || !DNC::ReadMaxTimeOf(self, maxT) || maxT <= 0.f || p.day > maxT) {
        const uint32_t n = ++g_malformed;
        if (n <= 5 || (n % 100) == 0)
            UE_LOGW("time_sync: streamed clock out of range (t=%.1f d=%.1f day %d; the day ends at %.1f) -- "
                    "dropped (#%u)", p.totalTime, p.day, p.dayZ, maxT, n);
        return;
    }
    ApplyClockSnapshot(self, p, maxT);
    ++g_appliedSince;
    const auto now = Clock::now();
    if (now >= g_nextStreamLine) {  // confirms convergence and the stream's rate, every 10 s
        g_nextStreamLine = now + std::chrono::seconds(10);
        UE_LOGI("time_sync: applied STREAM host clock totalTime=%.1f day=%.1f day number %d (samples since the "
                "last line: %u)", p.totalTime, p.day, p.dayZ, g_appliedSince);
        g_appliedSince = 0;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // The install fanout calls this every pump tick, which is the retry until the cycle class loads.
    if (g_tickObserved || !DNC::EnsureResolved()) return;
    void* fn = DNC::TickFunction();
    if (!fn) {
        UE_LOGW("time_sync: daynightCycle_C::ReceiveTick not found -- a client's clock cannot be parked");
        g_tickObserved = true;  // a class that loaded without it never gains it
        return;
    }
    if (!GT::RegisterPreObserver(fn, &OnCycleTickPre)) {
        static bool s_said = false;  // retried every pump tick; said once
        if (!s_said) UE_LOGW("time_sync: the cycle tick's pre-observer did not register (table full?) -- retrying");
        s_said = true;
        return;
    }
    g_tickObserved = true;
    UE_LOGI("time_sync: the client's clock is parked at the cycle's own tick (pre-observer on ReceiveTick)");
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    // HOST: read the clock every tick and hand the net thread a sample when one is due. Cheap -- a
    // few cached field reads.
    coop::net::TimeSyncPayload p{};
    double abs = 0.0;
    float maxT = 0.f;
    if (!MakePayload(p, abs, maxT)) return;
    const auto now = Clock::now();
    const bool moved = g_sentAbs < 0.0 || std::fabs(abs - g_sentAbs) >= maxT / (2.0 * kGameMinutesPerDay);
    if (!moved && now - g_sentAt < kRestInterval) return;
    s->SendHostClock(p);
    g_sentAbs = abs;
    g_sentAt = now;
}

bool HoldsCycle(void* cycle) {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() && s->role() == coop::net::Role::Client && !DNC::IsMenuCycle(cycle);
}

int32_t LastHostDayZ() { return g_lastHostDayZ.load(std::memory_order_acquire); }

void OnDisconnect() {
    g_lastHostDayZ.store(-1, std::memory_order_release);
    g_sentAbs = -1.0;
    g_sentAt = Clock::time_point{};
    EndBackRun();
    g_haveLastAbs = false;
    g_scaleOverrides = g_malformed = g_appliedSince = 0;
    g_nextStreamLine = Clock::time_point{};
    if (void* cycle = g_heldCycle.Get()) {
        // Hand the clock back: 1.0 is the game's own running value, the one its rewind restores. The
        // day runs on from the last sample, and the delivery latch self-heals at the next midnight.
        DNC::WriteTimeScaleOf(cycle, 1.0f);
        UE_LOGI("time_sync: restore -- client time scale back to 1.0");
    }
    g_heldCycle.Reset();
}

}  // namespace coop::time_sync
