// coop/dev/set_clock.cpp -- see coop/dev/set_clock.h.

#include "coop/dev/set_clock.h"

#include "coop/dev/dev_gate.h"
#include "ue_wrap/daynightcycle.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"

namespace coop::dev::set_clock {

namespace DNC = ue_wrap::daynightcycle;
namespace GT  = ue_wrap::game_thread;

bool ReadCurrent(int& dayOut, float& fracOut) {
    float total = 0.f, day = 0.f, scale = 0.f, maxT = 0.f;
    if (!DNC::ReadClock(total, day, scale)) return false;
    if (!DNC::ReadMaxTime(maxT) || maxT <= 0.f) return false;
    dayOut = static_cast<int>(day);
    float frac = total / maxT;
    if (frac < 0.f) frac = 0.f;
    if (frac > 1.f) frac = 1.f;
    fracOut = frac;
    return true;
}

void SetDay(int day) {
    if (!coop::dev_gate::Allowed()) {
        UE_LOGW("set_clock: SetDay REFUSED -- dev features are disabled while connected as a client");
        return;
    }
    if (day < 0) day = 0;
    GT::Post([day] {
        float total = 0.f, curDay = 0.f, scale = 0.f;
        if (!DNC::ReadClock(total, curDay, scale)) {
            UE_LOGW("set_clock: SetDay -- world clock not resolved (world up?)");
            return;
        }
        // Keep the current time-of-day (totalTime) + TimeScale; only advance the Day counter. time_sync
        // (host) broadcasts the new clock to clients on its next poll.
        DNC::ApplyClock(total, static_cast<float>(day), scale);
        UE_LOGI("set_clock: Day set to %d (totalTime/timeScale preserved)", day);
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
        // Map the fraction to a within-day totalTime; keep the current Day + TimeScale. The cycle's own
        // ReceiveTick re-derives the sun from the new totalTime. time_sync broadcasts it to clients.
        DNC::ApplyClock(frac * maxT, day, scale);
        UE_LOGI("set_clock: time-of-day set to %.3f of day (totalTime=%.1f / MaxTime=%.1f, day %.0f)",
                frac, frac * maxT, maxT, day);
    });
}

}  // namespace coop::dev::set_clock
