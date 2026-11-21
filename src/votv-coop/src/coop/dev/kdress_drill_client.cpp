// coop/dev/kdress_drill_client.cpp -- the client's tick, the bind of its copy of Omega C, and its halves of the put-on and the
// take-off arms; see coop/dev/kdress_drill.h. The pat, born and join arms are kdress_drill_client_arms.cpp.
//
// The put-on is the player's own: the glasses taken into the hand with `Hold Object`, then the Omega's playerUsedOn with them,
// asked of the host like any player's. The client reads its copy and its hand once its request record has closed (the replay
// ran and its commit left), and says what it read. The take-off is the equipment slot's own upgradeTake; the client says at
// once whether it asked the host, then waits for the host's word to reach its copy.

#include "kdress_drill_detail.h"

#include "coop/net/protocol.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_request.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <cstdint>
#include <string>

namespace coop::dev::kdress_drill::detail {

namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace HB  = coop::hand_item;
namespace PAR = coop::prop_act_request;

namespace {

int g_bind = 0;   // 0 the host's targets line, 1 the copy bound and at rest, 2 done

// The replies this client took as refused, all reasons.
int RefusedAll() {
    int n = 0;
    for (uint8_t r = 1; r < coop::net::kPropActReasons; ++r) n += static_cast<int>(PAR::dev::RefusedCount(static_cast<coop::net::PropActReason>(r)));
    return n;
}

// ---- the put-on ----

int g_put = 0, g_win = 0, g_itemEid = 0, g_refusedBase = 0;
ue_wrap::CachedObjRef g_item;

void PutStep(int to) {
    g_put = to;
    g_win = 0;
    Go();
}

// ---- the take-off ----

int g_off = 0, g_nearBase = 0;

void OffStep(int to) {
    g_off = to;
    g_win = 0;
    Go();
}

}  // namespace

bool ClientBindC() {
    if (g_bind == 0) {
        if (!ClientMayStart()) return false;
        std::string rest;
        if (!Rest("targets ", &rest)) {
            Late("the host's targets line never came");
            return false;
        }
        if (!ParseInts(rest, 1, &g_eidC)) {
            Invalid("the host's targets line could not be read");
            return false;
        }
        g_bind = 1;
        Go();
        return false;
    }
    if (g_bind == 1) {
        g_c.Set(OmegaOf(g_eidC));
        if (!g_c.Get() || !AtRest(g_c.Get())) {
            Late("the client never had its copy of Omega C at rest");
            return false;
        }
        g_bind = 2;
        Go();
    }
    return true;
}

void PutOnClient() {
    void* const me = coop::players::Registry::Get().Local();
    if (!me || !R::IsLive(me)) return;
    switch (g_put) {
    case 0:
        if (!ClientBindC()) return;
        Fact("Omega C eid=%d stands here wearing [%s]", g_eidC, DripText(g_c.Get()).c_str());
        Say("c-ready");
        PutStep(1);
        return;
    case 1: {
        std::string rest;
        if (!Rest("item ", &rest)) {
            Late("the host never said the glasses are here");
            return;
        }
        if (!ParseInts(rest, 1, &g_itemEid)) return Invalid("the host's item line could not be read");
        PutStep(2);
        return;
    }
    case 2:   // this client's copy may bind after the host's line arrives: it is looked up again until it stands here
        g_item.Set(LiveProp(g_itemEid));
        if (!g_item.Get()) {
            Late("this client's copy of the glasses never stood here");
            return;
        }
        PutStep(3);
        return;
    case 3: {
        void* const item = g_item.Get();
        if (!item) return Invalid("this client's copy of the glasses is gone");
        // The player's own hand-taking verb: `Hold Object` with `manual` set, the call the hand key makes on what it looks at.
        bool collected = false;
        const bool ran = E::CallMainPlayerHoldObject(me, item, collected);
        Fact("Hold Object on the glasses (%ls) ran=%d collected=%d; a request to the host open=%u", R::ClassNameOf(item).c_str(),
             ran ? 1 : 0, collected ? 1 : 0, static_cast<unsigned>(PAR::dev::PendingId()));
        if (!ran) return Invalid("Hold Object did not run");
        PutStep(4);
        return;
    }
    case 4: {
        if (PAR::dev::PendingId() != 0) {
            Late("the host never answered the hand-taking");
            return;
        }
        void* const hand = HB::LocalHandActor();
        if (!(hand && HB::OwnAnnounced(R::ClassNameOf(hand).c_str()))) {
            if (++g_win >= kReadTicks) Invalid("the hand holds nothing the lane announced after the verb");
            return;
        }
        Fact("the hand holds %p (%ls)", hand, R::ClassNameOf(hand).c_str());
        Say("c-held");   // after the hold's own commit on the one ordered lane: the host reads its counters from here
        PutStep(5);
        return;
    }
    case 5:
        if (!Seen("go")) {
            Late("the host never said go");
            return;
        }
        PutStep(6);
        return;
    case 6: {
        void* const c = g_c.Get();
        if (!c) return Invalid("this client's copy of Omega C is gone");
        g_refusedBase = RefusedAll();
        // A fresh ask, as a player's use of the held glasses: the drill does not use the replay's latch.
        const bool ran = UseOn(me, c);
        const bool opened = PAR::dev::PendingId() != 0;
        Fact("the player's playerUsedOn on C ran=%d, a request to the host open=%d", ran ? 1 : 0, opened ? 1 : 0);
        if (!ran) return Invalid("playerUsedOn did not run");
        if (!opened) return Invalid("the put-on asked the host nothing: the game's own refusal ran or the watch is dead");
        PutStep(7);
        return;
    }
    case 7: {   // the record closes when the replay ran and its commit left
        if (PAR::dev::PendingId() != 0) {
            Late("the host never answered the put-on");
            return;
        }
        void* const c = g_c.Get();
        if (!c) return Invalid("this client's copy of Omega C is gone");
        const int wears = DripHas(c, kRow) ? 1 : 0, empty = HandEmpty(me) ? 1 : 0, refused = RefusedAll() - g_refusedBase;
        Fact("after the put-on's record closed: the copy wears [%s], the hand is %s, refused replies %d", DripText(c).c_str(),
             empty ? "empty" : "not empty", refused);
        Say("c-put " + std::to_string(wears) + " " + std::to_string(empty) + " " + std::to_string(refused));
        Finish();
        return;
    }
    }
}

void TakeOffClient() {
    void* const me = coop::players::Registry::Get().Local();
    if (!me || !R::IsLive(me)) return;
    switch (g_off) {
    case 0:
        if (!ClientBindC()) return;
        OffStep(1);
        return;
    case 1: {   // the host's put-on reached this copy as its word (the 272's apply)
        if (!DripHas(g_c.Get(), kRow) || ItemClass(kRow).empty()) {
            Late("this client's copy of Omega C never wore the glasses the host put on");
            return;
        }
        g_nearBase = NearCount(ItemClass(kRow).c_str(), g_c.Get());
        Fact("Omega C eid=%d wears [%s]; %d glasses items stand beside it", g_eidC, DripText(g_c.Get()).c_str(), g_nearBase);
        Say("c-ready");
        OffStep(2);
        return;
    }
    case 2: {
        if (!Seen("go")) {
            Late("the host never said go");
            return;
        }
        void* const c = g_c.Get();
        if (!c) return Invalid("this client's copy of Omega C is gone");
        // The equipment slot's own take: `upgradeTake(item)` on the Omega, which runs unequipItem.
        ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(c), L"upgradeTake"));
        const R::FName item = ue_wrap::fname_utils::StringToFName(kRow);
        const bool ran = f.valid() && f.SetRaw(L"item", &item, sizeof(item)) && ue_wrap::Call(c, f);
        const bool opened = PAR::dev::PendingId() != 0;
        Fact("upgradeTake '%ls' on C ran=%d, a request to the host open=%d", kRow, ran ? 1 : 0, opened ? 1 : 0);
        if (!ran) return Invalid("upgradeTake did not run");
        Say(std::string("c-asked ") + (opened ? "1" : "0"));
        if (!opened) return Finish();   // the host judges what it was told: nothing
        OffStep(3);
        return;
    }
    case 3:
        if (PAR::dev::PendingId() != 0) {
            Late("the host never answered the take-off");
            return;
        }
        OffStep(4);
        return;
    case 4:   // the host's word reaches this copy
        if (DripHas(g_c.Get(), kRow)) {
            Late("this client's copy of Omega C never lost the row it took off");
            return;
        }
        Fact("this client's copy of Omega C lacks the row: [%s]", DripText(g_c.Get()).c_str());
        OffStep(5);
        return;
    case 5: {   // the dropped item's copy binds here
        const int now = NearCount(ItemClass(kRow).c_str(), g_c.Get());
        if (now < g_nearBase + 1) {
            Late("the glasses item never stood beside this client's Omega");
            return;
        }
        if (++g_win < kReadTicks) return;
        const int delta = NearCount(ItemClass(kRow).c_str(), g_c.Get()) - g_nearBase;
        Fact("%d glasses items stand beside this client's copy of Omega C after the take-off", delta);
        Say("c-off-done " + std::to_string(delta));
        Finish();
        return;
    }
    }
}

void ClientTick() {
    switch (ArmOf()) {
    case Arm::PutOn:   return PutOnClient();
    case Arm::TakeOff: return TakeOffClient();
    case Arm::Pat:     return PatClient();
    case Arm::Born:    return BornClient();
    case Arm::Join:    return JoinClient();
    case Arm::Off:     return;
    }
}

void ResetClient() {
    g_bind = 0;
    g_put = g_win = g_itemEid = g_refusedBase = 0;
    g_item.Reset();
    g_off = g_nearBase = 0;
    ResetClientArms();
}

}  // namespace coop::dev::kdress_drill::detail
