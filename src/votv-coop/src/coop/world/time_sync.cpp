// coop/world/time_sync.cpp -- see coop/world/time_sync.h. Host-authoritative world-clock sync.

#include "coop/world/time_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/world/day_edge.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/world/daynightcycle.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

namespace coop::time_sync {
namespace {

namespace DNC = ue_wrap::daynightcycle;
namespace GT  = ue_wrap::game_thread;
namespace R   = ue_wrap::reflection;
namespace SG  = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};
// The host's day number as the last applied sample carried it; -1 before the first.
std::atomic<int32_t> g_lastHostDayZ{-1};
bool g_tickObserved = false;  // the pre-observer on the cycle's tick is registered (once per process)
bool g_resetWatched = false;  // game mode 5's reset is watched (once per process)

// HOST: when a sample is due. One is sent when the clock has moved half a game minute since the last
// one sent, so consecutive samples cross at most one minute boundary -- a lost or merged sample, or a
// host frame longer than about 78 ms in the shared sleep, can skip one -- and at the latest after the
// resting interval. Measured on the clock itself (the
// day number times maxTime, plus `day`), whatever moved it: the sleep's time dilation, the difficulty,
// the day-length rule, a rewind or a set clock, which is sent at once.
constexpr double kGameMinutesPerDay = 1440.0;
constexpr auto   kRestInterval = std::chrono::milliseconds(500);
double g_sentAbs = -1.0;
Clock::time_point g_sentAt{};

// CLIENT, game thread. The cycle whose time scale we hold -- each world brings its own, and the one
// held last is handed back on disconnect -- the last applied sample, which the parked cycle presents
// at every tick, the backward run in progress against it, and the counts behind the rate-limited
// lines.
ue_wrap::CachedObjRef g_heldCycle;
bool     g_haveHeld = false;
coop::net::TimeSyncPayload g_held{};
uint32_t g_backRun = 0;         // consecutive samples that stepped back
double   g_backRunUnits = 0.0;  // and how far, in all
uint32_t g_backRuns = 0;        // backward runs ended since the connect
uint32_t g_scaleOverrides = 0;
uint32_t g_clockOverrides = 0;
LocalWrites g_localWrites{};    // the same writes, by what wrote over them
uint32_t g_malformed = 0;
uint32_t g_appliedSince = 0;  // samples since the last convergence line, which reports them every 10 s
Clock::time_point g_nextStreamLine{};

// CLIENT: game mode 5's master sets the cycle's `day` to 0, waits a second and loops, on every peer. Its
// begin-play runs one pass of that loop inline beside six flows of its own (the needs restore, the
// ambience, four spawners), and the loop's wait resumes its ubergraph at the loop's entry, whenever the
// master was spawned; the other flows' waits resume at entries of their own. The host's reset reaches a
// client in its samples, so a client refuses that one resume: the loop ends after the begin-play's pass,
// which the hold takes back, and the other flows run on. The entry is a byte offset in the cooked
// Blueprint, as the wall-attachable's and the door's entry constants are: a recook moves it, and the
// install line says the one it refuses.
constexpr int     kTagModeReset = 0x54534d52;  // 'TSMR'
constexpr int32_t kModeResetResume = 68;       // the Delay's resume in ExecuteUbergraph_halloweenMaster
void*   g_modeResetFn = nullptr;               // the ubergraph the entry parameter's offset was read from
int32_t g_modeResetEntryOff = -1;

SG::Verdict OnModeResetResumePre(const SG::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client || !call.locals) return SG::Verdict::Run;
    if (call.function != g_modeResetFn) {
        g_modeResetFn = call.function;
        g_modeResetEntryOff = R::FindParamOffset(call.function, L"EntryPoint");
    }
    if (g_modeResetEntryOff < 0) return SG::Verdict::Run;
    int32_t entry = 0;
    std::memcpy(&entry, call.locals + g_modeResetEntryOff, sizeof(entry));
    if (entry != kModeResetResume) return SG::Verdict::Run;
    UE_LOGI("time_sync: game mode 5's reset of the day every second refused on this client (%p) -- the host's "
            "samples carry its own", call.object);
    return SG::Verdict::Cancel;
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
// menu or the purple wisp's rewind, is overwritten before the tick reads it, and named. True on the
// tick that parked a new cycle.
bool HoldTimeScale(void* cycle, float s) {
    if (!g_heldCycle.Is(cycle)) {
        g_heldCycle.Set(cycle);
        if (s != 0.f) DNC::WriteTimeScaleOf(cycle, 0.f);
        UE_LOGI("time_sync: client clock parked (time scale %.2f -> 0; the host's samples move it)", s);
        return true;
    }
    if (s == 0.f) return false;
    DNC::WriteTimeScaleOf(cycle, 0.f);
    const uint32_t n = ++g_scaleOverrides;
    if (n <= 5 || (n % 50) == 0)
        UE_LOGW("time_sync: the client's clock rate was set to %.2f on this machine -- held at 0, the host owns "
                "the clock (#%u)", s, n);
    return false;
}

// CLIENT: name a write of the clock on this machine that the lane found at a tick and wrote over, with
// the last sample (`over` is g_held) or a new one. Counted by which, for instruments.
void NameLocalWrite(float t, float d, int32_t dayZ, const coop::net::TimeSyncPayload& over, bool bySample) {
    ++(bySample ? g_localWrites.met : g_localWrites.held);
    const uint32_t n = ++g_clockOverrides;
    if (n <= 5 || (n % 50) == 0)
        UE_LOGW("time_sync: the client's clock was written on this machine (day=%.2f totalTime=%.1f day number %d) "
                "-- %s (day=%.2f day number %d), the host owns the clock (#%u)", d, t, dayZ,
                bySample ? "a new host sample written over it" : "held at the host's last sample", over.day,
                over.dayZ, n);
}

// CLIENT: between samples, the parked cycle presents the last applied one. A write on this machine
// since the last tick -- the cheat menu's day buttons, the one pass of game mode 5's reset a master's
// begin-play runs -- is overwritten, and named, before the tick reads it, so no tick the pre-observer
// precedes rolls this machine's `day`; the cycle's call from the gamemode's begin-play runs the tick
// body once per world without it, on the loaded `day`, which is below maxTime. A new world's cycle
// takes the sample at its park, unnamed: its clock is the save's. Every correction latches the 6 am
// order, as a sample's does: a new world's slot, parked before its first sample, can carry it open.
void HoldClock(void* cycle, float t, float d, bool parked) {
    if (!g_haveHeld) return;
    const bool clock = (t != g_held.totalTime || d != g_held.day);
    if (clock) DNC::ApplyClockOf(cycle, g_held.totalTime, g_held.day);
    void* slot = DNC::SaveSlotOfCycle(cycle);
    int32_t sh = 0, sm = 0, sz = -1;
    const bool dayNumber = DNC::ReadSavedTimeOf(slot, sh, sm, sz) && sz != g_held.dayZ &&
                           DNC::WriteSavedDayOf(slot, g_held.dayZ);
    if (clock || dayNumber || parked) DNC::LatchDailyDeliveryOf(slot);
    if ((clock || dayNumber) && !parked) NameLocalWrite(t, d, sz, g_held, false);
}

// CLIENT: say a backward run once, when it ends. The host's rewind runs its clock back for an hour,
// so every sample inside it steps back; a clock set back steps back once, and game mode 5 sets it back
// every second, so the first five runs and every 50th are said.
void EndBackRun() {
    if (g_backRun == 0) return;
    const uint32_t n = ++g_backRuns;
    if (n <= 5 || (n % 50) == 0)
        UE_LOGI("time_sync: the host's clock went back %.2f units over %u sample(s) -- its rewind or a set clock "
                "(#%u)", g_backRunUnits, g_backRun, n);
    g_backRun = 0;
    g_backRunUnits = 0.0;
}

// CLIENT: write one host sample into the parked cycle and its own save slot, and hold it until the
// next. The day number is written only when it moved, and never the hour and minute: those are the
// client's settime's, so its pulses fire as the host's samples cross each minute.
void ApplyClockSnapshot(void* cycle, const coop::net::TimeSyncPayload& p, float maxT) {
    DNC::ApplyClockOf(cycle, p.totalTime, p.day);
    void* slot = DNC::SaveSlotOfCycle(cycle);
    int32_t sh = 0, sm = 0, sz = 0;
    if (DNC::ReadSavedTimeOf(slot, sh, sm, sz) && sz != p.dayZ && DNC::WriteSavedDayOf(slot, p.dayZ))
        UE_LOGI("time_sync: day number %d -> %d, the host's", sz, p.dayZ);
    // The host's midnight, crossed between two samples of this session: this client performs its own
    // share of the rollover. A day number the host already had when the session began is the save's.
    if (g_haveHeld && p.dayZ > g_held.dayZ) coop::day_edge::OnHostDayEdge(cycle, slot, g_held.dayZ, p.dayZ);
    const double abs = static_cast<double>(p.dayZ) * maxT + p.day;
    const double heldAbs = static_cast<double>(g_held.dayZ) * maxT + g_held.day;
    if (g_haveHeld && abs < heldAbs) {
        ++g_backRun;
        g_backRunUnits += heldAbs - abs;
    } else {
        EndBackRun();
    }
    g_held = p;
    g_haveHeld = true;
    g_lastHostDayZ.store(p.dayZ, std::memory_order_release);
    // The 6 am order latch: func_newHour still runs on the client's pulse, and the game's own reset
    // of the flag is part of the midnight the client no longer rolls; a save load could reset it.
    DNC::LatchDailyDeliveryOf(slot);
}

// CLIENT: the cycle is about to tick. Park it and write the newest host sample into it, if one has
// arrived since the last was read, or else hold the last: nothing reads the stream before a joined
// world's cycle does, so that world's first tick reads the host's clock at a zero rate.
void OnCycleTickPre(void* self, void* /*function*/, void* /*params*/) {
    if (!GT::IsGameThread() || !HoldsCycle(self)) return;
    auto* s = g_session.load(std::memory_order_acquire);  // set, since HoldsCycle answered yes
    float t = 0, d = 0, rate = 0;
    if (!DNC::ReadClockOf(self, t, d, rate)) return;
    const bool parked = HoldTimeScale(self, rate);
    coop::net::TimeSyncPayload p{};
    bool isNew = false;
    if (!s->TryGetHostClock(p, &isNew) || !isNew) {
        HoldClock(self, t, d, parked);
        return;
    }
    // A `day` past the day's end would run this client's own midnight at the tick -- the game's roll
    // test is `day > maxTime` -- and the host wraps inside its own tick, so no sample it makes has one.
    // The sample's format was checked at receive (coop/net's ValidateClock); the day length is this
    // cycle's, so the test against it is here.
    float maxT = 0.f;
    if (!DNC::ReadMaxTimeOf(self, maxT) || maxT <= 0.f || p.day > maxT) {
        const uint32_t n = ++g_malformed;
        if (n <= 5 || (n % 100) == 0)
            UE_LOGW("time_sync: streamed clock out of range (t=%.1f d=%.1f day %d; the day ends at %.1f) -- "
                    "dropped (#%u)", p.totalTime, p.day, p.dayZ, maxT, n);
        HoldClock(self, t, d, parked);
        return;
    }
    // A write on this machine since the last tick meets the new sample, which writes over it: named too.
    if (g_haveHeld && !parked) {
        int32_t sh = 0, sm = 0, sz = -1;
        const bool number = DNC::ReadSavedTimeOf(DNC::SaveSlotOfCycle(self), sh, sm, sz) && sz != g_held.dayZ;
        if (number || t != g_held.totalTime || d != g_held.day) NameLocalWrite(t, d, sz, p, true);
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
    if (!g_resetWatched) {
        // Keyed by names, so it holds from the class's first load, in whichever world brings it.
        g_resetWatched = true;
        if (SG::WatchClassName(L"halloweenMaster_C", L"ExecuteUbergraph_halloweenMaster", kTagModeReset,
                               &OnModeResetResumePre, nullptr))
            UE_LOGI("time_sync: a client refuses game mode 5's reset at its ubergraph entry %d (the loop's resume)",
                    kModeResetResume);
        else
            UE_LOGE("time_sync: the watch on game mode 5's reset did not register -- a client runs its own");
    }
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

LocalWrites LocalWritesFound() { return g_localWrites; }

void OnDisconnect() {
    g_lastHostDayZ.store(-1, std::memory_order_release);
    g_sentAbs = -1.0;
    g_sentAt = Clock::time_point{};
    EndBackRun();
    g_haveHeld = false;
    g_held = coop::net::TimeSyncPayload{};
    g_scaleOverrides = g_clockOverrides = g_malformed = g_appliedSince = g_backRuns = 0;
    g_localWrites = LocalWrites{};
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
