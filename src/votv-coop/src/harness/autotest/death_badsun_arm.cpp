// harness/autotest/death_badsun_arm.cpp -- see harness/autotest/death_badsun_arm.h.

#include "harness/autotest/death_badsun_arm.h"

#include "harness/autotest/death_state_probe.h"
#include "harness/autotest/gt_task.h"

#include "coop/dev/director/director.h"
#include "coop/dev/set_clock.h"
#include "coop/player/death_revive.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/world/bad_sun.h"

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>

namespace harness::autotest {
namespace {

namespace BS = ue_wrap::bad_sun;
namespace DIR = coop::director;
namespace P = ue_wrap::profile;

// The clock the Bad Sun drill's `spawn` arm sets before it spawns the event (badsun_drill_lane.cpp, kAt0800):
// inside the copy's 7 to 18 test window, so it goes active at its first test.
constexpr float kAt0800 = (8 * 60) / 1440.f;

// Standing at the KPP: the walk's reach, and the distance under which no walk is made (the arm's solo host
// starts a New Game there).
constexpr float kAtKppCm = 400.f;
// Bounds, not waits: the walk to the gate, and the event's first kill. Each phase ends on its own line.
constexpr int kWalkBoundSec = 300;
constexpr uint64_t kKillBoundMs = 240000;
// What the sheet asks: one revive and no second death in this long after the first death.
constexpr uint64_t kAfterDeathMs = 90000;
constexpr uint64_t kSampleMs = 250;
// The landing's tolerance, as the death drill's D7.
constexpr float kLandingTolCm = 500.f;


void Verdict(const char* word, const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (word[0] == 'P') UE_LOGI("death_test: BADSUN VERDICT %s -- %s", word, buf);
    else UE_LOGW("death_test: BADSUN VERDICT %s -- %s", word, buf);
}

DeathSnapshot Sample() {
    auto out = std::make_shared<DeathSnapshot>();
    RunOnGameThread([out] { *out = ReadDeathState(); }, 8000);
    return *out;
}

// True when the player stands at the KPP, walked there by the director when it was not.
bool StandAtKpp() {
    const DeathSnapshot s = Sample();
    if (s.haveLoc) {
        const float dx = s.locX - P::name::kKPPSpawnX, dy = s.locY - P::name::kKPPSpawnY,
                    dz = s.locZ - P::name::kKPPSpawnZ;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) <= kAtKppCm) return true;
    }
    DIR::DirectorGoal goal;
    goal.targetPos = {P::name::kKPPSpawnX, P::name::kKPPSpawnY, P::name::kKPPSpawnZ};
    goal.reachCm = kAtKppCm;
    UE_LOGI("death_test: BADSUN walking to the KPP with the director (%d s bound)", kWalkBoundSec);
    DIR::ControlManager mgr;
    DIR::AddWalkToProcesses(mgr, goal);
    mgr.Run(goal, kWalkBoundSec);
    if (!goal.reached) UE_LOGW("death_test: BADSUN the walk to the KPP ended unreached (%s)", goal.failReason);
    return goal.reached;
}

}  // namespace

void RunDeathBadSunArm(bool isClient) {
    UE_LOGI("death_test: BADSUN arm -- role=%s no-bed=%d", isClient ? "CLIENT" : "HOST",
            coop::shelter_spot::BedLookupDisabled() ? 1 : 0);
    if (isClient) {
        // A client's own Spawn Bad Sun is refused by the lane (the drill's `cheat` arm), and the clock is the host's.
        Verdict("INCONCLUSIVE", "the arm starts the event with the host's own verbs; run it on the host");
        return;
    }
    if (!StandAtKpp()) {
        Verdict("INCONCLUSIVE", "the player could not be walked to the KPP");
        return;
    }

    // The event, by the Bad Sun drill's own spawn path: a fresh world holds none, the clock goes to 08:00 and the
    // gamemode's Spawn Bad Sun runs.
    auto started = std::make_shared<int>(0);
    RunOnGameThread([started] {
        if (BS::CountLive(nullptr, 1) != 0) { *started = 2; return; }
        if (!coop::dev::set_clock::ApplyTimeFraction(kAt0800)) { *started = 3; return; }
        *started = BS::CallSpawn() ? 1 : 4;
    });
    if (*started != 1) {
        Verdict("INCONCLUSIVE", "the event did not start (%s)",
                *started == 2 ? "a Bad Sun is already live" : *started == 3 ? "the clock set was refused"
                : *started == 4 ? "Spawn Bad Sun did not run" : "the game thread did not answer");
        return;
    }
    UE_LOGI("death_test: BADSUN clock to 08:00 and Spawn Bad Sun called; waiting for the event's first kill");

    // Death edges, sampled at the cadence: a death holds `dead` for about ten seconds, so none is missed.
    uint64_t tFirst = 0;
    int deaths = 0;
    bool wasDead = false, graded = false, targetBed = false, reviveOk = false;
    char landing[320] = {};
    const uint64_t t0 = ::GetTickCount64();
    for (;;) {
        const uint64_t now = ::GetTickCount64();
        if (tFirst == 0 ? now - t0 > kKillBoundMs : now - tFirst > kAfterDeathMs) break;
        const DeathSnapshot s = Sample();
        const bool dead = s.haveState && s.dead;
        if (dead && !wasDead) {
            ++deaths;
            if (tFirst == 0) tFirst = now;
            UE_LOGI("death_test: BADSUN death %d at +%llu ms", deaths,
                    static_cast<unsigned long long>(now - (tFirst ? tFirst : t0)));
        }
        wasDead = dead;
        // The first death's revive has landed: dead is false again. The target and the landing are read now.
        if (deaths >= 1 && !dead && !graded && s.haveState) {
            graded = true;
            auto kind = std::make_shared<int>(-1);
            auto j = std::make_shared<LandingJudgement>();
            auto ok = std::make_shared<bool>(false);
            RunOnGameThread([s, kind, j, ok] {
                *kind = static_cast<int>(coop::death_revive::LastReviveTarget().kind);
                *ok = coop::death_revive::LastReviveSucceeded();
                *j = JudgeRevivedLanding(s, kLandingTolCm);
            });
            targetBed = *kind == static_cast<int>(coop::shelter_spot::Kind::Bed);
            reviveOk = *ok;
            std::snprintf(landing, sizeof(landing), "%s", j->detail);
            UE_LOGI("death_test: BADSUN the first revive: ok=%d kind=%s -- %s", reviveOk ? 1 : 0,
                    coop::shelter_spot::KindName(static_cast<coop::shelter_spot::Kind>(*kind)), landing);
        }
        ::Sleep(kSampleMs);
    }

    if (deaths == 0) {
        Verdict("INCONCLUSIVE", "the event never killed the player in %llu s",
                static_cast<unsigned long long>(kKillBoundMs / 1000));
        return;
    }
    const bool pass = graded && reviveOk && targetBed && deaths == 1;
    Verdict(pass ? "PASS" : "FAIL",
            "deaths in %llu s after the first=%d (want 1), revive graded=%d ok=%d target=%s (want bed); %s",
            static_cast<unsigned long long>(kAfterDeathMs / 1000), deaths, graded ? 1 : 0, reviveOk ? 1 : 0,
            targetBed ? "bed" : "not-bed", landing);
}

}  // namespace harness::autotest
