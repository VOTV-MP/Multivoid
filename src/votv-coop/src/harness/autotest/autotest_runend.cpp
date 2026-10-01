// harness/autotest/autotest_runend.cpp -- the drill for the run-ending seam.

// None of the five run-endings that reach the menu without setting `dead` is reproducible on
// demand: the flesh NPC has to catch you, the birch has to take you. So this drill does not
// reproduce an ending -- it drives the one call they all pass through. The CANCEL direction is
// end to end: dispatch `lib_C::loadLevel("menu", ..., <a non-UI author>)` for real in a live
// session, then assert the world survived, the cancel was counted and the revive ran. The ALLOW
// direction is by judgement, because dispatching the player's own quit would end the run and
// take the drill with it; `JudgeMenuTravel` is the seam's own classification with no side
// effect, so the test asserts the decision instead of keeping a second copy of it. A third claim
// rides along: four of the five endings pause the world, and the travel they asked for is what
// used to un-pause it, so the drill pauses before the cancel and asserts un-paused after.
// Solo host. Env VOTVCOOP_RUN_RUNEND_DRILL=1; the lines are tagged [RUNEND].

#include "harness/autotest.h"

#include "harness/autotest/death_state_probe.h"
#include "harness/autotest/gt_task.h"

#include "coop/player/death_revive.h"
#include "coop/player/run_end_travel.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"
#include "coop/text/i18n.h"

#include <windows.h>

#include <atomic>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

namespace E   = ue_wrap::engine;
namespace GT  = ue_wrap::game_thread;
namespace R   = ue_wrap::reflection;
namespace P   = ue_wrap::profile;
namespace RET = coop::player::run_end_travel;

struct Tally {
    int pass = 0;
    int fail = 0;
};
Tally g_v;

void Check(bool ok, const char* name, const char* why) {
    (ok ? g_v.pass : g_v.fail)++;
    if (ok) UE_LOGI("[RUNEND] PASS %s -- %s", name, why);
    else    UE_LOGE("[RUNEND] FAIL %s -- %s", name, why);
}

const wchar_t* JudgementName(RET::Judgement j) {
    switch (j) {
        case RET::Judgement::RunNoSession:   return L"RunNoSession";
        case RET::Judgement::RunPlayerAsked: return L"RunPlayerAsked";
        case RET::Judgement::RunNoRevive:    return L"RunNoRevive";
        case RET::Judgement::Cancel:         return coop::i18n::TrW(L"Cancel");
    }
    return L"?";
}

// Dispatch lib_C::loadLevel("menu", "", true, author) the way a run-ending does. Returns false if
// the call could not be built at all -- which is itself a finding, since it means the seam is
// watching something the game does not have.
bool DispatchMenuTravel(void* author) {
    void* cdo = R::FindClassDefaultObject(L"lib_C");
    if (!cdo) { UE_LOGE("[RUNEND] lib_C CDO not found -- cannot drive the travel"); return false; }
    void* fn = R::FindFunction(R::ClassOf(cdo), L"loadLevel");
    if (!fn) { UE_LOGE("[RUNEND] lib_C::loadLevel not found -- cannot drive the travel"); return false; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) { UE_LOGE("[RUNEND] loadLevel param frame invalid"); return false; }
    R::FName menu = ue_wrap::fname_utils::StringToFName(L"menu");
    if (menu.ComparisonIndex == 0 && menu.Number == 0) {
        UE_LOGE("[RUNEND] StringToFName(\"menu\") returned NAME_None -- not dispatching");
        return false;
    }
    // `option` is left as the frame built it (an empty FString): the seam never reads it, and the
    // body does not run on the path this drill asserts.
    const bool okArgs = f.SetRaw(L"level", &menu, sizeof(menu)) &&
                        f.Set<bool>(L"clearSubArea", true) &&
                        f.SetRaw(L"__WorldContext", &author, sizeof(author));
    if (!okArgs) { UE_LOGE("[RUNEND] loadLevel argument write failed"); return false; }
    return ue_wrap::Call(cdo, f);
}

}  // namespace

void RunRunEndDrill() {
    UE_LOGI("[RUNEND] starting (waiting 60 s: world, possession, a live session)");
    ::Sleep(60000);

    // The seam's own readiness, first: every later verdict is meaningless without it.
    Check(RET::WatchInstalled(), "A1 watch",
          RET::WatchInstalled() ? "lib_C::loadLevel is watched at the script-body gate"
                                : "the watch never registered -- nothing below judges anything");
    Check(coop::death_revive::ReviveAvailable(), "A2 revive-available",
          coop::death_revive::ReviveAvailable()
              ? "verbs resolved, dead offset known, session running"
              : "the revive is unavailable, so the seam is deliberately passing travels through");

    // The ALLOW direction, by judgement: the pause menu's own quit must stay allowed. Asserting it
    // by dispatch would end the run and take the rest of the drill with it.
    RunOnGameThread([] {
        void* menu = R::FindObjectByClass(L"ui_menu_C");
        if (!menu) {
            Check(false, "B1 quit-allowed", "no live ui_menu_C to judge -- the pause menu widget "
                                            "is created at gamemode init, so this is a finding");
            return;
        }
        const RET::Judgement j = RET::JudgeMenuTravel(menu);
        const bool ok = j == RET::Judgement::RunPlayerAsked;
        UE_LOGI("[RUNEND] judgement for ui_menu_C = %ls", JudgementName(j));
        Check(ok, "B1 quit-allowed",
              ok ? "a menu travel authored by the pause menu is ALLOWED: the player can still quit"
                 : "the player's own quit would be REFUSED -- that traps them in the world");
    });

    // The CANCEL direction, end to end. The gamemode stands in for gameover_C / npc_angryErieFlesh_C
    // / birch_C / SKEL_C / tutorWall_spikes_C: what the seam reads is that the author is not the
    // pause menu, which is true of all five.
    const unsigned long long cancelledBefore = RET::TravelsCancelled();
    const unsigned long long menuSeenBefore  = RET::MenuTravelsSeen();
    // Outputs are shared_ptr, never `&local`: RunOnGameThread's wait is bounded but the posted
    // task is not, and the ProcessEvent detour's transparent bypass parks the queue for up to 30 s
    // -- which is exactly what a travel this drill failed to cancel would arm.
    auto worldBefore = std::make_shared<std::wstring>();
    auto worldAfter  = std::make_shared<std::wstring>();
    auto pausedAfter = std::make_shared<bool>(true);
    auto aliveAfter  = std::make_shared<bool>(false);
    auto atKppAfter  = std::make_shared<bool>(false);

    RunOnGameThread([worldBefore] {
        if (void* w = R::FindObjectByClass(P::name::WorldClass)) *worldBefore = R::ToString(R::NameOf(w));
        // Four of the five endings pause before their delay; the cancelled travel is what no
        // longer un-pauses. Reproduce that state through the game's own verb.
        E::SetGamePaused(true);
        UE_LOGI("[RUNEND] world '%ls', paused=%d -- dispatching a run-ending travel authored by "
                "the gamemode (the stand-in for the five endings)",
                worldBefore->c_str(), E::IsGamePaused() ? 1 : 0);
        void* gm = R::FindObjectByClass(P::name::GamemodeClass);
        if (!gm) { UE_LOGE("[RUNEND] no mainGamemode_C to author with"); return; }
        DispatchMenuTravel(gm);
    });

    // The revive runs on the next pump task, and the screen cleanup after it; a second is plenty.
    ::Sleep(3000);

    const bool readBack = RunOnGameThread([worldAfter, pausedAfter, aliveAfter, atKppAfter] {
        if (void* w = R::FindObjectByClass(P::name::WorldClass)) *worldAfter = R::ToString(R::NameOf(w));
        *pausedAfter = E::IsGamePaused();
        // The shared reader, not a second hand-rolled one: its pawn is the registry's LOCAL
        // player, where this drill's own copy took the first mainPlayer_C the array offered --
        // correct on a solo host with no puppets and wrong the moment one exists.
        const DeathSnapshot s = ReadDeathState();
        *aliveAfter = s.haveState && !s.dead;
        if (s.haveLoc) {
            const float dx = s.locX - P::name::kKPPSpawnX, dy = s.locY - P::name::kKPPSpawnY;
            *atKppAfter = (dx * dx + dy * dy) < (600.f * 600.f);
        }
    });
    if (!readBack) {
        // Every C-row below reads this snapshot. Unread is not "false": an unread window satisfies
        // world-kept and player-alive by default, which is the shape of an instrument grading
        // itself green.
        UE_LOGE("[RUNEND] the post-cancel read never reached the game thread -- the rows below "
                "would be asserted on unread state");
        Check(false, "C0 read-back", "the game thread did not answer within the budget");
    }

    const unsigned long long cancelled = RET::TravelsCancelled() - cancelledBefore;
    const unsigned long long menuSeen  = RET::MenuTravelsSeen() - menuSeenBefore;

    Check(menuSeen >= 1, "C1 seam-saw",
          menuSeen >= 1 ? "the dispatched travel reached the seam"
                        : "the travel never reached the seam -- the watch is not on the path, so "
                          "nothing below proves anything about the cancel");
    Check(cancelled >= 1, "C2 cancelled",
          cancelled >= 1 ? "the run-ending travel was REFUSED"
                         : "the travel was ALLOWED -- a run-ending nobody asked for would end the "
                           "session for every peer");
    Check(!worldAfter->empty() && *worldAfter == *worldBefore, "C3 world-kept",
          (!worldAfter->empty() && *worldAfter == *worldBefore)
              ? "the same UWorld is still live: nothing travelled"
              : "the world CHANGED -- the travel went through");
    Check(*aliveAfter, "C4 player-alive",
          *aliveAfter ? "the local player reads not-dead after the cancel"
                     : "the player is dead or unreadable after the cancel");
    Check(!*pausedAfter, "C5 un-paused",
          !*pausedAfter ? "the pause the ending set is gone: the revive disposed of it"
                       : "the world is STILL PAUSED -- the travel that used to un-pause it was "
                         "cancelled and nothing took over");
    Check(*atKppAfter, "C6 at-kpp",
          *atKppAfter ? "the revive repositioned the player to the KPP"
                     : "the player is not at the KPP -- the revive's teleport did not land");

    UE_LOGI("[RUNEND] SEAM -- watch=%d travelsSeen=%llu menuTravels=%llu cancelled=%llu "
            "lastReviveOk=%d",
            RET::WatchInstalled() ? 1 : 0, RET::TravelsSeen(), RET::MenuTravelsSeen(),
            RET::TravelsCancelled(), coop::death_revive::LastReviveSucceeded() ? 1 : 0);
    UE_LOGI("[RUNEND] VERDICT %s (%d checks passed, %d failed)",
            g_v.fail == 0 ? "PASS" : "FAIL", g_v.pass, g_v.fail);
    UE_LOGI("[RUNEND] DONE");
}

DWORD WINAPI RunEndDrillThread(LPVOID) {
    RunRunEndDrill();
    return 0;
}

}  // namespace harness::autotest
