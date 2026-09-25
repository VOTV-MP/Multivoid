// coop/world/day_edge.cpp -- see coop/world/day_edge.h.

#include "coop/world/day_edge.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/world/daynightcycle.h"
#include "ue_wrap/world/event_list.h"
#include "ue_wrap/world/game_mode.h"
#include "ue_wrap/world/profile.h"

namespace coop::day_edge {
namespace {

namespace DNC = ue_wrap::daynightcycle;
namespace PRF = ue_wrap::profile;

constexpr int32_t kInsomniacNights = 7;  // the rollover's own count for the achievement
constexpr int32_t kSandboxDay = 30;      // and its day for the sandbox one

// Whether the midnights that took the day number from `from` to `to` include the one that reached `day`:
// the rollover tests its day number at each midnight, and a host can cross several between two samples.
bool Crosses(int32_t from, int32_t to, int32_t day) { return from < day && day <= to; }

// For the line: nothing when the achievement was not due, `yes` when due and progressed, `refused`
// when due and the call did not go through.
const char* Said(bool due, bool progressed, const char* yes, const char* refused) {
    return !due ? "" : progressed ? yes : refused;
}

}  // namespace

void OnHostDayEdge(void* cycle, void* saveSlot, int32_t from, int32_t to) {
    const int32_t days = to - from;
    if (days <= 0) return;
    const bool lived = PRF::AddDaysTotal(days);
    const bool musics = DNC::WriteAllMusicsOf(saveSlot, true);
    // The day-number achievements, in game mode 0 only, each tested on its own as the rollover tests them:
    // sandbox at any midnight from day 30, alphaFinish at the one that reaches the story's last event day.
    const bool mode0 = ue_wrap::game_mode::ReadLocal() == 0;
    int32_t storyEnd = -1;
    const bool sandboxDue = mode0 && to >= kSandboxDay;
    const bool sandbox = sandboxDue && PRF::ProgressAchievement(L"sandbox");
    const bool finishDue = mode0 && ue_wrap::event_list::ReadLastDay(storyEnd) && Crosses(from, to, storyEnd);
    const bool finish = finishDue && PRF::ProgressAchievement(L"alphaFinish");
    int32_t sleepless = -1, sleeplessAfter = -1;
    bool insomniacDue = false, insomniac = false;
    if (DNC::ReadSleeplessDaysOf(cycle, sleepless)) {
        sleeplessAfter = sleepless + days;
        DNC::WriteSleeplessDaysOf(cycle, sleeplessAfter);
        insomniacDue = Crosses(sleepless, sleeplessAfter, kInsomniacNights);
        insomniac = insomniacDue && PRF::ProgressAchievement(L"insomniac");
    }
    UE_LOGI("day_edge: the host's day %d -> %d; this client's share -- days lived +%d (%s), music flags %s, "
            "sleeplessDays %d -> %d%s%s%s", from, to, days, lived ? "written" : "UNRESOLVED",
            musics ? "set" : "UNRESOLVED", sleepless, sleeplessAfter,
            Said(insomniacDue, insomniac, ", insomniac", ", insomniac NOT progressed"),
            Said(sandboxDue, sandbox, ", sandbox", ", sandbox NOT progressed"),
            Said(finishDue, finish, ", alphaFinish", ", alphaFinish NOT progressed"));
}

}  // namespace coop::day_edge
