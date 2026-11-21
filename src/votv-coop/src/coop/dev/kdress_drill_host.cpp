// coop/dev/kdress_drill_host.cpp -- the host's tick, the stand of Omega C before the client's puppet, and the host's halves of
// the put-on and the take-off arms; see coop/dev/kdress_drill.h. The pat, born and join arms are kdress_drill_host_arms.cpp.
//
// The put-on is decided by the host's own read of its copy once its prop_act counters say the client's commit was taken; the
// take-off by its read at the client's `asked` line and once its counters say the ask was answered.

#include "kdress_drill_detail.h"

#include "coop/cheats/item_spawn.h"
#include "coop/dev/drill_ground.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_act_host.h"

#include "ue_wrap/core/log.h"

#include <algorithm>
#include <string>
#include <vector>

namespace coop::dev::kdress_drill::detail {

namespace DG  = coop::dev::drill_ground;
namespace IS  = coop::cheats::item_spawn;
namespace PAH = coop::prop_act_host;
using coop::net::PropActReason;
using Step = PAH::internal::CommitStep;

namespace {

constexpr uint8_t kSlot = 1;   // the one client the drill acts on

int g_stand = 0;   // 0 spawn C, 1 wait its id and rest, 2 stands

// The commits the host took of the client's, whatever their ending.
uint32_t CommitsTaken() {
    uint32_t n = 0;
    for (const Step s : {Step::FamilyApply, Step::Abort, Step::BadCommit, Step::TargetGone}) n += PAH::CommitsOf(kSlot, s);
    return n;
}

// ---- the put-on ----

int g_put = 0, g_quiet = 0;
std::vector<void*> g_itemBefore;     // the glasses that stood before the spawn: the new copy is the one not among them
int g_itemResult = 0, g_itemMade = 0;
ue_wrap::CachedObjRef g_item;
uint32_t g_commitBase = 0, g_applyBase = 0, g_abortBase = 0;
int g_clientSays[3] = {};            // wears, hand empty, refused replies

void PutStep(int to) {
    g_put = to;
    Go();
}

}  // namespace

bool HostStandC(coop::net::Session& s) {
    if (g_stand == 0) {
        if (!HostMayStart(s)) return false;
        coop::RemotePlayer& rp = coop::puppet_drive::Puppet(kSlot);
        if (!rp.valid() || !rp.actor()) {
            Late("the client's puppet never stood on the host");
            return false;
        }
        g_c.Set(SpawnOmega(rp.actor(), kCAheadCm, 0.f));
        if (!g_c.Get()) {
            Invalid("the world refused the Omega's spawn");
            return false;
        }
        Fact("spawned Omega C %.0f cm before the client's puppet", kCAheadCm);
        g_stand = 1;
        Go();
        return false;
    }
    if (g_stand == 1) {
        g_eidC = OmegaEid(g_c.Get());
        if (!g_eidC || !AtRest(g_c.Get())) {
            Late("Omega C never got an element id and came to rest");
            return false;
        }
        Fact("Omega C eid=%d is at rest", g_eidC);
        g_stand = 2;
        Go();
    }
    return true;
}

void PutOnHost(coop::net::Session& s) {
    switch (g_put) {
    case 0:
        if (!HostStandC(s)) return;
        Say("targets " + std::to_string(g_eidC));
        PutStep(1);
        return;
    case 1:
        if (!Seen("c-ready")) {
            Late("the client never said its copy of Omega C is here");
            return;
        }
        PutStep(2);
        return;
    case 2: {   // the glasses, spawned beside the client's puppet
        if (!CatalogReady()) return;
        const std::wstring cls = ItemClass(kRow);
        if (cls.empty()) return Invalid("the row names no class the game spawns");
        g_itemBefore = LiveOf(cls.c_str());
        g_itemMade = 0;
        g_itemResult = static_cast<int>(IS::Spawn(Narrow(kRow), 1, 1, false, &g_itemMade));
        PutStep(3);
        return;
    }
    case 3: {   // the new copy: the cheat spawn's actor can enter the object index a tick later
        void* item = nullptr;
        for (void* a : LiveOf(ItemClass(kRow).c_str()))
            if (std::find(g_itemBefore.begin(), g_itemBefore.end(), a) == g_itemBefore.end()) item = a;
        if (!item) {
            Late("the spawn of the glasses beside the client's puppet left no new copy");
            return;
        }
        g_item.Set(item);
        g_quiet = 0;
        Fact("spawned '%ls' beside the client's puppet: result=%d made=%d", kRow, g_itemResult, g_itemMade);
        PutStep(4);
        return;
    }
    case 4: {
        void* const item = g_item.Get();
        if (!item) return Invalid("the glasses died before the client held them");
        const int eid = PropEid(item);
        g_quiet = eid && DG::Quiet(item) ? g_quiet + 1 : 0;
        if (g_quiet < kQuietTicks) {
            Late("the glasses never got an element id and came to rest");
            return;
        }
        Say("item " + std::to_string(eid));
        Fact("the glasses eid=%d are at rest; said to the client", eid);
        PutStep(5);
        return;
    }
    case 5:   // the hold's own commit has been taken by now: the counters are read from here
        if (!Seen("c-held")) {
            Late("the client never said it holds the glasses");
            return;
        }
        g_commitBase = CommitsTaken();
        g_applyBase = PAH::CommitsOf(kSlot, Step::FamilyApply);
        g_abortBase = PAH::CommitsOf(kSlot, Step::Abort);
        Say("go");
        PutStep(6);
        return;
    case 6: {
        std::string rest;
        if (!Rest("c-put ", &rest)) {
            Late("the client never said what its put-on left");
            return;
        }
        if (!ParseInts(rest, 3, g_clientSays)) return Invalid("the client's put-on line could not be read");
        // The put-on's commit leaves the client before its line, so the host takes it first: its counters say when.
        if (g_clientSays[2] > 0)
            return Fail("the host refused the client's put-on of '%ls' (%d refused replies)", kRow, g_clientSays[2]);
        PutStep(7);
        return;
    }
    case 7: {
        if (CommitsTaken() <= g_commitBase) {
            Late("the host never took the client's commit");
            return;
        }
        void* const c = OmegaOf(g_eidC);
        if (!c) return Invalid("the host's copy of Omega C is gone");
        if (!g_clientSays[0])
            return Fail("the client's copy of Omega eid=%d does not wear '%ls' after its replay", g_eidC, kRow);
        if (!g_clientSays[1]) return Fail("the client's hand still holds the glasses after its put-on");
        if (PAH::CommitsOf(kSlot, Step::Abort) > g_abortBase)
            return Fail("the client's put-on aborted at its commit: the host wrote no row (the client's copy wears it)");
        if (!DripHas(c, kRow))
            return Fail("the host's copy of Omega eid=%d does not wear '%ls' after it took the client's commit (applied %u; "
                        "its copy wears [%s]; the client's copy wears it and its hand is empty)",
                        g_eidC, kRow, PAH::CommitsOf(kSlot, Step::FamilyApply) - g_applyBase, DripText(c).c_str());
        Pass("put-on: the client's copy wears the glasses and its hand is empty, and the host's copy wears them once the host "
             "took the commit");
        return;
    }
    }
}

// ---- the take-off ----

namespace {

int g_off = 0, g_win = 0;
int g_nearBase = 0;
uint32_t g_totalBase = 0, g_grantBase = 0;

void OffStep(int to) {
    g_off = to;
    g_win = 0;
    Go();
}

}  // namespace

void TakeOffHost(coop::net::Session& s) {
    void* const me = coop::players::Registry::Get().Local();
    switch (g_off) {
    case 0:
        if (!me || !HostStandC(s) || !CatalogReady()) return;
        if (!DressOmega(me, g_c.Get())) return;
        Say("targets " + std::to_string(g_eidC));
        Fact("the host dressed Omega C: it wears [%s]", DripText(g_c.Get()).c_str());
        OffStep(1);
        return;
    case 1: {
        if (!Seen("c-ready")) {
            Late("the client never said its copy of Omega C wears the glasses");
            return;
        }
        g_nearBase = NearCount(ItemClass(kRow).c_str(), g_c.Get());
        g_totalBase = PAH::AnsweredTotal(kSlot);
        g_grantBase = PAH::AnsweredFor(kSlot, PropActReason::Granted);
        Say("go");
        OffStep(2);
        return;
    }
    case 2: {   // the client's line says whether its upgradeTake opened a request
        std::string rest;
        if (!Rest("c-asked ", &rest)) {
            Late("the client never ran its take-off");
            return;
        }
        int opened = 0;
        if (!ParseInts(rest, 1, &opened)) return Invalid("the client's asked line could not be read");
        void* const c = OmegaOf(g_eidC);
        if (!c) return Invalid("the host's copy of Omega C is gone");
        if (opened == 0)
            return Fail("the host's copy of Omega eid=%d still wears '%ls' after the client's take-off: its upgradeTake asked "
                        "the host nothing", g_eidC, kRow);
        OffStep(3);
        return;
    }
    case 3: {   // the ask was answered: what the host did is final
        if (PAH::AnsweredTotal(kSlot) <= g_totalBase) {
            Late("the host never answered the client's take-off");
            return;
        }
        void* const c = OmegaOf(g_eidC);
        if (!c) return Invalid("the host's copy of Omega C is gone");
        if (PAH::AnsweredFor(kSlot, PropActReason::Granted) <= g_grantBase)
            return Fail("the host refused the client's take-off of '%ls'", kRow);
        if (DripHas(c, kRow))
            return Fail("the host granted the client's take-off of '%ls' but its copy of Omega eid=%d still wears it", kRow, g_eidC);
        OffStep(4);
        return;
    }
    case 4: {   // the item the game drops beside the Omega: one, and no more once the window has run
        const int now = NearCount(ItemClass(kRow).c_str(), g_c.Get());
        if (now < g_nearBase + 1) {
            Late("the glasses item never stood beside the host's Omega");
            return;
        }
        if (++g_win < kReadTicks) return;
        if (now != g_nearBase + 1)
            return Fail("%d glasses items stand beside the host's Omega after the take-off, one expected", now - g_nearBase);
        Fact("the host's copy lacks the row and one glasses item stands beside it");
        Say("h-off-ok");
        OffStep(5);
        return;
    }
    case 5: {
        std::string rest;
        if (!Rest("c-off-done ", &rest)) {
            Late("the client never said its copy lacks the row with the item beside it");
            return;
        }
        int delta = 0;
        if (!ParseInts(rest, 1, &delta)) return Invalid("the client's off-done line could not be read");
        if (delta != 1)
            return Fail("%d glasses items stand beside the client's copy of the Omega after the take-off, one expected", delta);
        Pass("take-off: the host answered the client's ask and took the glasses off its copy, and both copies lack the row with "
             "one glasses item beside the Omega on each peer");
        return;
    }
    }
}

void HostTick(coop::net::Session& s) {
    switch (ArmOf()) {
    case Arm::PutOn:   return PutOnHost(s);
    case Arm::TakeOff: return TakeOffHost(s);
    case Arm::Pat:     return PatHost(s);
    case Arm::Born:    return BornHost(s);
    case Arm::Join:    return JoinHost(s);
    case Arm::Off:     return;
    }
}

void ResetHost() {
    g_stand = 0;
    g_put = g_quiet = g_off = g_win = 0;
    g_itemBefore.clear();
    g_itemResult = g_itemMade = 0;
    g_item.Reset();
    g_commitBase = g_applyBase = g_abortBase = 0;
    g_clientSays[0] = g_clientSays[1] = g_clientSays[2] = 0;
    g_nearBase = 0;
    g_totalBase = g_grantBase = 0;
    ResetHostArms();
}

}  // namespace coop::dev::kdress_drill::detail
