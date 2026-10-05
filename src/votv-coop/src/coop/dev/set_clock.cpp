// coop/dev/set_clock.cpp -- see coop/dev/set_clock.h.

#include "coop/dev/set_clock.h"

#include "coop/dev/dev_gate.h"
#include "ue_wrap/world/daynightcycle.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace coop::dev::set_clock {

namespace DNC = ue_wrap::daynightcycle;
namespace GT  = ue_wrap::game_thread;

namespace {

// The menu's view of the clock. The F1 panel renders on the render thread and the cycle's fields
// are the game thread's, so the panel reads this snapshot and asks the game thread to refresh it,
// at most four times a second.
struct Snapshot { bool ok = false; int hour = 0, minute = 0, day = 0; float sunFrac = 0.f; };
std::mutex g_snapMu;
Snapshot g_snap;
std::atomic<bool> g_refreshPosted{false};
std::atomic<uint64_t> g_nextRefreshMs{0};

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Game thread: read the live clock into the snapshot.
void Refresh() {
    Snapshot n;
    int32_t h = 0, m = 0, d = 0;
    float total = 0.f, dayAcc = 0.f, scale = 0.f, maxT = 0.f;
    if (DNC::ReadTimeZ(h, m, d) && DNC::ReadClock(total, dayAcc, scale) && DNC::ReadMaxTime(maxT) &&
        maxT > 0.f) {
        n.ok = true;
        n.hour = h;
        n.minute = m;
        n.day = d + 1;  // the game's own display convention (savedtime.Z + 1; save_browser fix)
        float frac = total / maxT;
        if (frac < 0.f) frac = 0.f;
        if (frac > 1.f) frac = 1.f;
        n.sunFrac = frac;
    }
    std::lock_guard<std::mutex> lk(g_snapMu);
    g_snap = n;
}

}  // namespace

bool ReadCurrent(int& hourOut, int& minuteOut, int& dayOut, float& sunFracOut) {
    const uint64_t now = NowMs();
    if (now >= g_nextRefreshMs.load(std::memory_order_relaxed) &&
        !g_refreshPosted.exchange(true, std::memory_order_acq_rel)) {
        g_nextRefreshMs.store(now + 250, std::memory_order_relaxed);
        GT::Post([] {
            Refresh();
            g_refreshPosted.store(false, std::memory_order_release);
        });
    }
    std::lock_guard<std::mutex> lk(g_snapMu);
    if (!g_snap.ok) return false;
    hourOut = g_snap.hour;
    minuteOut = g_snap.minute;
    dayOut = g_snap.day;
    sunFracOut = g_snap.sunFrac;
    return true;
}

void SetClock(int day, int hour, int minute) {
    if (!coop::dev_gate::Allowed()) {
        UE_LOGW("set_clock: SetClock REFUSED -- dev features are disabled while connected as a client");
        return;
    }
    if (day < 1) day = 1;  // displayed day is 1-based
    if (hour < 0) hour = 0;
    if (hour > 23) hour = 23;
    if (minute < 0) minute = 0;
    if (minute > 59) minute = 59;
    GT::Post([day, hour, minute] {
        int32_t curH = 0, curM = 0, curD = 0;
        if (!DNC::ReadTimeZ(curH, curM, curD)) {
            UE_LOGW("set_clock: SetClock -- world clock not resolved (world up?)");
            return;
        }
        const int rawZ = day - 1;  // displayed day -> the save's day number
        // INSTANT and COMPLETE, from the two pieces the cycle derives everything else from:
        //   day              -- the within-day accumulator the sun derives from AND the midnight
        //                       cascade fires on, so writing it moves the lighting; totalTime,
        //                       the absolute elapsed clock, is kept equal to it so the two never
        //                       disagree about where in the day we are;
        //   savedtime's day  -- the day number, where it lives; the cycle copies it into the
        //                       named clock every tick, which is why a write of that never held.
        // The next tick rebuilds the named clock and settime raises its pulses; a forward day jump
        // fires skipped scheduled events natively, through settime's own walk. Connected clients
        // converge on the next streamed sample, which carries the day number.
        float total = 0.f, dayAcc = 0.f, scale = 0.f, maxT = 0.f;
        if (!DNC::ReadClock(total, dayAcc, scale) || !DNC::ReadMaxTime(maxT) || maxT <= 0.f) {
            UE_LOGW("set_clock: SetClock -- cycle accumulators not resolved");
            return;
        }
        const float frac = (hour * 60 + minute) / 1440.0f;
        const float newTotal = frac * maxT;
        if (!DNC::WriteSavedDay(rawZ)) {
            UE_LOGW("set_clock: SetClock -- the save's day number not resolved");
            return;
        }
        DNC::ApplyClock(newTotal, newTotal);
        UE_LOGI("set_clock: clock set Day %d %02d:%02d -> Day %d %02d:%02d INSTANT "
                "(day number %d -> %d; totalTime %.1f -> %.1f of %.1f; sun re-derives this tick; "
                "a forward day jump fires skipped scheduled events natively)",
                curD + 1, curH, curM, day, hour, minute, curD, rawZ, total, newTotal, maxT);
    });
}

bool ApplyTimeFraction(float frac) {
    if (!coop::dev_gate::Allowed()) {
        UE_LOGW("set_clock: SetTimeFraction REFUSED -- dev features disabled while a client");
        return false;
    }
    if (frac < 0.f)   frac = 0.f;
    if (frac > 0.999f) frac = 0.999f;  // keep strictly < MaxTime so it can't trip the day-roll threshold
    float total = 0.f, day = 0.f, scale = 0.f, maxT = 0.f;
    if (!DNC::ReadClock(total, day, scale) || !DNC::ReadMaxTime(maxT) || maxT <= 0.f) {
        UE_LOGW("set_clock: SetTimeFraction -- world clock not resolved (world up?)");
        return false;
    }
    // The accumulators alone: the next tick rebuilds the named clock from `day`, so the HUD and the
    // lighting move together. Same day; the two accumulators are kept equal, as in SetClock.
    const int minutes = static_cast<int>(frac * 1440.0f);
    DNC::ApplyClock(frac * maxT, frac * maxT);
    UE_LOGI("set_clock: sun+clock set to %.3f of day (%02d:%02d; totalTime=%.1f / MaxTime=%.1f)",
            frac, minutes / 60, minutes % 60, frac * maxT, maxT);
    return true;
}

void SetTimeFraction(float frac) {
    GT::Post([frac] { ApplyTimeFraction(frac); });
}

}  // namespace coop::dev::set_clock
