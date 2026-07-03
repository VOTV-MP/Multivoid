// coop/dev/set_clock.cpp -- see coop/dev/set_clock.h.

#include "coop/dev/set_clock.h"

#include "coop/dev/dev_gate.h"
#include "ue_wrap/daynightcycle.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"

#include <cstdint>

namespace coop::dev::set_clock {

namespace DNC = ue_wrap::daynightcycle;
namespace GT  = ue_wrap::game_thread;

bool ReadCurrent(int& hourOut, int& minuteOut, int& dayOut, float& sunFracOut) {
    int32_t h = 0, m = 0, d = 0;
    if (!DNC::ReadTimeZ(h, m, d)) return false;
    float total = 0.f, dayAcc = 0.f, scale = 0.f, maxT = 0.f;
    if (!DNC::ReadClock(total, dayAcc, scale)) return false;
    if (!DNC::ReadMaxTime(maxT) || maxT <= 0.f) return false;
    hourOut = h;
    minuteOut = m;
    dayOut = d + 1;  // the game's own display convention (savedtime.Z + 1; save_browser fix)
    float frac = total / maxT;
    if (frac < 0.f) frac = 0.f;
    if (frac > 1.f) frac = 1.f;
    sunFracOut = frac;
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
        const int rawZ = day - 1;  // displayed day -> the scheduler/save day-Z
        DNC::WriteTimeZ(hour, minute, rawZ);
        UE_LOGI("set_clock: clock set Day %d %02d:%02d -> Day %d %02d:%02d (timeZ.Z %d -> %d; the "
                "next minute pulse feeds it through settime; a forward day jump fires skipped "
                "scheduled events natively)",
                curD + 1, curH, curM, day, hour, minute, curD, rawZ);
    });
}

void SetTimeFraction(float frac) {
    if (!coop::dev_gate::Allowed()) {
        UE_LOGW("set_clock: SetTimeFraction REFUSED -- dev features disabled while a client");
        return;
    }
    if (frac < 0.f)   frac = 0.f;
    if (frac > 0.999f) frac = 0.999f;  // keep strictly < MaxTime so it can't trip the day-roll threshold
    GT::Post([frac] {
        float total = 0.f, day = 0.f, scale = 0.f, maxT = 0.f;
        if (!DNC::ReadClock(total, day, scale) || !DNC::ReadMaxTime(maxT) || maxT <= 0.f) {
            UE_LOGW("set_clock: SetTimeFraction -- world clock not resolved (world up?)");
            return;
        }
        // Sun only: map the fraction to a within-day totalTime; keep the Day accumulator +
        // TimeScale + the named clock (timeZ). The cycle's ReceiveTick re-derives the sun.
        DNC::ApplyClock(frac * maxT, day, scale);
        UE_LOGI("set_clock: sun set to %.3f of day (totalTime=%.1f / MaxTime=%.1f; named clock untouched)",
                frac, frac * maxT, maxT);
    });
}

}  // namespace coop::dev::set_clock
