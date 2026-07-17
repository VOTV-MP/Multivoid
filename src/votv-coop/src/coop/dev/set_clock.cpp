// coop/dev/set_clock.cpp -- see coop/dev/set_clock.h.

#include "coop/dev/set_clock.h"

#include "coop/dev/dev_gate.h"
#include "ue_wrap/world/daynightcycle.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

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
        // INSTANT + COMPLETE (user 2026-07-03: the old timeZ-only write left the sun
        // where it was until the next minute pulse "fed it through" -- and the pulse
        // rebuilds timeZ from the accumulators, so the write could even snap back).
        // The full native clock state is three pieces:
        //   totalTime -- the within-day [0, MaxTime) clock the sun/moon re-derive from
        //                EVERY ReceiveTick -> writing it moves the lighting this frame;
        //   day       -- the within-day midnight-cascade accumulator: keep it equal to
        //                totalTime so the next natural midnight stays exactly one
        //                day-remainder away (never crosses MaxTime spuriously);
        //   timeZ     -- the NAMED clock (HUD + settime scheduler + save persistence).
        // The next minute pulse persists via settime; a forward day jump fires skipped
        // scheduled events natively (settime's own walk). Connected clients converge on
        // the next time_sync correction (<= 2 s), which now carries timeZ too (v96).
        float total = 0.f, dayAcc = 0.f, scale = 0.f, maxT = 0.f;
        if (!DNC::ReadClock(total, dayAcc, scale) || !DNC::ReadMaxTime(maxT) || maxT <= 0.f) {
            UE_LOGW("set_clock: SetClock -- cycle accumulators not resolved");
            return;
        }
        const float frac = (hour * 60 + minute) / 1440.0f;
        const float newTotal = frac * maxT;
        DNC::WriteTimeZ(hour, minute, rawZ);
        DNC::ApplyClock(newTotal, newTotal, scale);
        UE_LOGI("set_clock: clock set Day %d %02d:%02d -> Day %d %02d:%02d INSTANT "
                "(timeZ.Z %d -> %d; totalTime %.1f -> %.1f of %.1f; sun re-derives this tick; "
                "a forward day jump fires skipped scheduled events natively)",
                curD + 1, curH, curM, day, hour, minute, curD, rawZ, total, newTotal, maxT);
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
        int32_t h = 0, m = 0, dz = 0;
        if (!DNC::ReadTimeZ(h, m, dz)) {
            UE_LOGW("set_clock: SetTimeFraction -- timeZ not resolved");
            return;
        }
        // COMPLETE set (2026-07-03): sun AND the named clock together -- the old sun-only
        // write desynced the HUD clock from the lighting, and the next minute pulse
        // rebuilt everything from the accumulators anyway. Same day; the within-day
        // accumulator follows totalTime (see SetClock).
        const int minutes = static_cast<int>(frac * 1440.0f);
        DNC::WriteTimeZ(minutes / 60, minutes % 60, dz);
        DNC::ApplyClock(frac * maxT, frac * maxT, scale);
        UE_LOGI("set_clock: sun+clock set to %.3f of day (%02d:%02d; totalTime=%.1f / MaxTime=%.1f)",
                frac, minutes / 60, minutes % 60, frac * maxT, maxT);
    });
}

}  // namespace coop::dev::set_clock
