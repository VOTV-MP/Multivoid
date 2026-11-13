// coop/dev/prop_act_drill_client_steps.cpp -- the act drill's client steps that wait on the host's word after the act: an
// opened MRE's contents and the poison bite, and a snack arm's copies. Split from prop_act_drill_client.cpp (the step
// machine) with no change of behaviour. See coop/dev/prop_act_drill.h.

#include "coop/dev/prop_act_drill.h"
#include "prop_act_drill_client_steps.h"

#include "coop/dev/drill_chat_pace.h"
#include "coop/element/registry.h"

#include "ue_wrap/actors/effects.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::dev::prop_act_drill {
using namespace detail;
namespace EL = coop::element;

void StepAwaitContents(const Row& row, void* player, uint64_t now) {
        // The host rolled the contents on its copy and says, behind their births and FoodStates on the same ordered
        // lane, how many there are and how many it poisoned: the line is the event this step decides on, and what this
        // client holds when it reads it is final. The host set the MRE's poisonChance to 1, so one content is poisoned, and
        // its poison rides the FoodState that follows its spawn.
        if (now < g_cNextMs) return;
        g_cNextMs = now + kPollEveryMs;
        std::string said;
        if (!HostSaid(ContentsHead(row), &said)) {
            if (CExpired(kContentsBoundMs)) ClientInvalid(row, "the host never said the contents were sent");
            return;
        }
        const size_t sentN = static_cast<size_t>(std::strtoul(said.c_str() + (said.find("n=") == std::string::npos ? 0 : said.find("n=") + 2), nullptr, 10));
        const size_t hostPoisoned = static_cast<size_t>(std::strtoul(said.c_str() + (said.find("poisoned=") == std::string::npos ? 0 : said.find("poisoned=") + 9), nullptr, 10));
        void* poisoned = nullptr;
        bool waiting = false;
        g_freshContents = FoodMirrorsNotIn(g_foodsBefore, nullptr, &poisoned, &waiting);
        // A FoodState that still waits for its copy has not been applied: what the contents show is not final until it has.
        if (waiting) {
            if (CExpired(kContentsBoundMs)) ClientInvalid(row, "a FoodState for a content never drained from the pending set");
            return;
        }
        if (g_freshContents != sentN) {
            ClientFail(row, "the host rolled " + std::to_string(sentN) + " contents, " + std::to_string(g_freshContents) +
                                " reached this client");
            return;
        }
        if (!poisoned) {
            ClientFail(row, hostPoisoned ? "no poisoned content reached this client: the host poisoned " +
                                               std::to_string(hostPoisoned) + " and its FoodStates carried none"
                                         : "no poisoned content reached this client: the host rolled none");
            return;
        }
        // Its bite: the game's own `bitten` adds the foodPoison effect when both fields read above 0, as a press on it does.
        std::vector<ue_wrap::effects::Entry> list;
        g_poisonBefore = ue_wrap::effects::List(&list) ? FoodPoisonLive(list) : 0;
        if (!CallBitten(poisoned, player)) {
            ClientFail(row, "the poisoned content's bite could not run here");
            return;
        }
        CEnter(CStep::AwaitPoison);
        return;
}

void StepAwaitPoison(const Row& row, void* /*player*/, uint64_t now) {
        // The bite call has returned: the effect it adds is there at this poll, or the bite added none. Decided at this
        // poll, with no wait.
        if (now < g_cNextMs) return;
        g_cNextMs = now + kPollEveryMs;
        std::vector<ue_wrap::effects::Entry> list;
        if (!ue_wrap::effects::List(&list) || FoodPoisonLive(list) <= g_poisonBefore) {
            ClientFail(row, "the bite of a poisoned content added no foodPoison here");
            return;
        }
        ue_wrap::effects::Remove(L"foodPoison");   // the drill's next rows eat: leave the player as it was
        const std::string what = "asked, granted, replayed; the mirror is gone, " + std::to_string(g_freshContents) +
                                 " host-born food mirror(s) arrived and a bite of the poisoned one added foodPoison";
        ClientPass(row, what.c_str());
        return;
}

void StepAwaitCopy(const Row& row, void* /*player*/, uint64_t now) {
        if (now < g_cNextMs) return;
        g_cNextMs = now + kPollEveryMs;
        std::string why;
        // The host's `settled` line is the event: it follows every birth, record and destroy the host sent for the act on
        // one ordered lane, so once it is read what this client shows is final, a mismatch is a FAIL and no wait remains.
        // A bound that runs out before it, with the copies still short, only says INVALID.
        switch (SnackClientCopy(row, g_mirror, g_ceid, g_readyRest, HostSaid(SnackSettledHead(row), nullptr), &why)) {
        case Judged::Wait:
            if (CExpired(kCopyBoundMs)) ClientInvalid(row, "the host's settled line never came: " + why);
            return;
        case Judged::Fail:
            ClientFail(row, why);
            return;
        case Judged::Pass:
            ClientPass(row, why.c_str());
            return;
        }
        return;
}

void StepAwaitUpright(const Row& row, void* player, uint64_t now) {
    // The next row's target is placed by this player's puppet, so the host waits for this line before it spawns it: a
    // player lying in a ragdoll (mainPlayer.isRagdoll) has not finished, and the line waits for the flag to clear. The
    // wait ends on that event; only a bound that runs out first says INVALID. The verdict of the step is already said.
    static bool sSaid = false;
    bool ragdoll = false, dead = false;
    const bool read = ue_wrap::engine::ReadMainPlayerRagdollState(player, ragdoll, dead);
    if (read && ragdoll && !dead) {
        if (!sSaid) {
            sSaid = true;
            std::vector<ue_wrap::effects::Entry> fx;
            std::string live;
            if (!ue_wrap::effects::List(&fx)) live = " unread";
            else
                for (const auto& e : fx)
                    if (e.live) live += " " + std::string(e.name.begin(), e.name.end());
            UE_LOGI("%s c%d %s: the player is ragdolled after %s; live effects:%s", kTag, kClientSlot,
                    PhaseName().c_str(), row.name, live.empty() ? " none" : live.c_str());
        }
        if (CExpired(kStandBoundMs)) {
            UE_LOGW("%s c%d %s INVALID -- %s: the player stayed ragdolled", kTag, kClientSlot, PhaseName().c_str(), row.name);
            RigSay(End::Invalid, std::string(row.name) + ": the player stayed ragdolled");
            g_cstep = CStep::Done;
        }
        return;
    }
    if (!read) UE_LOGW("%s c%d %s: the player's ragdoll state could not be read; it is taken as standing", kTag, kClientSlot,
                       PhaseName().c_str());
    if (now < g_nextSayMs) return;   // a client's chat passes the host's rate limit only at the drills' spacing
    sSaid = false;
    Say(UprightHead(row));
    g_nextSayMs = now + coop::dev::kTalkEveryMs;
    ++g_crow;
    CEnter(CStep::AwaitReady);
}

}  // namespace coop::dev::prop_act_drill
