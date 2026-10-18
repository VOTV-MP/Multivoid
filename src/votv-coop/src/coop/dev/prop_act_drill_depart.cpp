// coop/dev/prop_act_drill_depart.cpp -- both halves of the act drill's phase `depart`: c1 takes a host prop, holds its
// commit and is killed by the rig (--rejoin), so the host holds a reservation for a slot that left; the newcomer in
// that slot forges the dead life's commit and then takes the prop honestly. See coop/dev/prop_act_drill.h.
//
//   HOST -- spawns a crowbar, sees c1 reserve it, sees the departure release the reservation and tally it, says the
//           newcomer line once slot 1 is world-ready again, tallies the forged commit as bad with the crowbar
//           standing, and judges the honest take as one destroy commit.
//   c1   -- life 1 pockets the crowbar with its commit held and prints the rig's marker. Life 2 forges the commit,
//           waits for the host to have tallied it, then pockets the crowbar honestly.

#include "prop_act_drill_depart.h"

#include "coop/dev/prop_act_drill.h"

#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat_pace.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_host.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/prop_hold.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

namespace coop::dev::prop_act_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace EL  = coop::element;
namespace PAH = coop::prop_act_host;
namespace PAR = coop::prop_act_request;
using ue_wrap::FVector;
using Tally = PAH::Tally;

constexpr int kSlot = kClientSlot;   // the one client, in both lives

// ---- host (judge) ------------------------------------------------------------------------------------------------

struct HBase {
    uint32_t heldAtDisconnect = 0, badCommit = 0, destroys = 0;
};

enum class HStage : uint8_t { Arm, Spawn, Announce, AwaitReserved, AwaitGone, AwaitNewcomer, AwaitBad, AwaitDone,
                              Judge, Done };
HStage g_hs = HStage::Arm;
uint64_t g_hsMs = 0, g_hNextMs = 0;
ue_wrap::CachedObjRef g_target;
EL::ElementId g_eid = EL::kInvalidId;
HBase g_base;

void HEnter(HStage s) {
    g_hs = s;
    g_hsMs = NowMs();
}

bool HExpired(uint64_t bound) { return NowMs() - g_hsMs > bound; }

void HostFail(const std::string& why) {
    UE_LOGW("%s host %s FAIL -- %s", kTag, kDepartRow.name, why.c_str());
    g_hs = HStage::Done;
}

// c1 said its step failed: the phase ends there rather than on a bound.
bool ClientFailed() {
    std::string rest;
    if (!ClientSaid(kSlot, DoneHead(kDepartRow), &rest) || rest == "ok") return false;
    HostFail("c1 reported: " + rest);
    return true;
}

// The target is on the host and alive as the eid names it.
bool TargetStands() { return g_target.Alive() && EL::LivePropActor(g_eid) != nullptr; }

void HostStep(coop::net::Session* s) {
    // Between c1's reservation and the newcomer the slot is empty; every other stage needs the client present.
    const bool slotMayBeEmpty = g_hs == HStage::Arm || g_hs == HStage::AwaitReserved || g_hs == HStage::AwaitGone ||
                                g_hs == HStage::AwaitNewcomer;
    if (!slotMayBeEmpty && !s->HasPeerConn(kSlot)) {
        HostFail("the client left before the verdict");
        return;
    }
    switch (g_hs) {
    case HStage::Arm:
        // The join and its late window are over, so what is spawned reaches the client as a live spawn.
        if (!s->IsSlotWorldReady(kSlot) || !coop::prop_snapshot::IsBracketClosed(kSlot) ||
            coop::join_window_baseline::IsLateWindowOpen(kSlot) || !coop::players::Registry::Get().Local() ||
            !ClientPuppetReady())
            return;
        UE_LOGI("%s host depart armed on slot %d", kTag, kSlot);
        HEnter(HStage::Spawn);
        return;
    case HStage::Spawn: {
        void* actor = nullptr;
        if (const char* why = SpawnTarget(kDepartRow, 0.f, &actor)) {
            HostFail(why);
            return;
        }
        g_target.Set(actor);
        HEnter(HStage::Announce);
        return;
    }
    case HStage::Announce: {
        void* const actor = g_target.Get();
        if (!actor) {
            HostFail("the spawned prop was gone before it was announced");
            return;
        }
        g_eid = EL::Registry::Get().EidForActor(actor);
        if (g_eid == EL::kInvalidId) {
            if (HExpired(kAnnounceBoundMs)) HostFail("the spawned prop was never announced");
            return;
        }
        g_base.heldAtDisconnect = PAH::TallyOf(Tally::HeldAtDisconnect);
        Say(ReadyHead(kDepartRow) + std::to_string(static_cast<unsigned>(g_eid)));
        UE_LOGI("%s host depart said ready: eid %u", kTag, static_cast<unsigned>(g_eid));
        HEnter(HStage::AwaitReserved);
        return;
    }
    case HStage::AwaitReserved:
        if (ClientFailed()) return;
        if (coop::prop_hold::ReservedEid(kSlot) != static_cast<uint32_t>(g_eid)) {
            if (HExpired(kDoneBoundMs)) HostFail("c1 never reserved the target");
            return;
        }
        UE_LOGI("%s host depart: c1 holds the reservation, waiting for it to leave", kTag);
        HEnter(HStage::AwaitGone);
        return;
    case HStage::AwaitGone: {
        // The rig kills c1 on its marker; the host sees the slot go, tally it and release the reservation.
        const uint32_t left = PAH::TallyOf(Tally::HeldAtDisconnect) - g_base.heldAtDisconnect;
        if (left == 0) {
            if (HExpired(kDoneBoundMs)) HostFail("the departure never tallied a reservation held at disconnect");
            return;
        }
        if (left != 1 || coop::prop_hold::ReservationCount(kSlot) != 0 || !TargetStands()) {
            HostFail("the departure tallied " + std::to_string(left) + " reservations, left " +
                     std::to_string(coop::prop_hold::ReservationCount(kSlot)) + " standing, or the target is gone");
            return;
        }
        UE_LOGI("%s host depart: the departure released the reservation (held_at_disconnect +1), the target stands",
                kTag);
        HEnter(HStage::AwaitNewcomer);
        return;
    }
    case HStage::AwaitNewcomer:
        if (!s->HasPeerConn(kSlot) || !s->IsSlotWorldReady(kSlot) || !coop::prop_snapshot::IsBracketClosed(kSlot) ||
            coop::join_window_baseline::IsLateWindowOpen(kSlot)) {
            if (HExpired(kDoneBoundMs)) HostFail("no newcomer reached world-ready in the slot");
            return;
        }
        g_base.badCommit = PAH::TallyOf(Tally::BadCommit);
        g_base.destroys = PAH::CommitsOf(kSlot, PAH::internal::CommitStep::Destroy);
        Say(NewcomerHead() + std::to_string(static_cast<unsigned>(g_eid)));
        UE_LOGI("%s host depart said the newcomer line", kTag);
        HEnter(HStage::AwaitBad);
        return;
    case HStage::AwaitBad: {
        if (ClientFailed()) return;
        const uint32_t bad = PAH::TallyOf(Tally::BadCommit) - g_base.badCommit;
        if (bad == 0) {
            if (HExpired(kDoneBoundMs)) HostFail("the newcomer's forged commit was never tallied bad");
            return;
        }
        if (bad != 1 || !TargetStands() ||
            PAH::CommitsOf(kSlot, PAH::internal::CommitStep::Destroy) != g_base.destroys) {
            HostFail("the forged commit tallied " + std::to_string(bad) + " bad commits, or it destroyed the target");
            return;
        }
        Say(BadSeenLine());
        UE_LOGI("%s host depart: the forged commit was tallied bad, the target stands", kTag);
        HEnter(HStage::AwaitDone);
        return;
    }
    case HStage::AwaitDone: {
        std::string rest;
        if (!ClientSaid(kSlot, DoneHead(kDepartRow), &rest)) {
            if (HExpired(kDoneBoundMs)) HostFail("the newcomer never said it was done");
            return;
        }
        if (rest != "ok") {
            HostFail("c1 reported: " + rest);
            return;
        }
        HEnter(HStage::Judge);
        return;
    }
    case HStage::Judge: {
        const uint32_t destroys = PAH::CommitsOf(kSlot, PAH::internal::CommitStep::Destroy) - g_base.destroys;
        const bool settled = destroys == 1 && EL::LivePropActor(g_eid) == nullptr &&
                             coop::prop_hold::ReservationCount(kSlot) == 0;
        if (!settled) {
            if (HExpired(kJudgeBoundMs))
                HostFail("the honest take left " + std::to_string(destroys) + " destroy commits, a live target or a reservation");
            return;
        }
        if (PAH::TallyOf(Tally::BadCommit) - g_base.badCommit != 1) {
            HostFail("the honest take was tallied bad");
            return;
        }
        UE_LOGI("%s host depart PASS -- the departure released the reservation, the forged commit was bad and "
                "destroyed nothing, the honest take destroyed the prop once", kTag);
        g_hs = HStage::Done;
        return;
    }
    case HStage::Done:
        return;
    }
}

// ---- client (c1) -------------------------------------------------------------------------------------------------

enum class CStage : uint8_t { Arm, Decide, Forge, AwaitBad, Resolve, Walk, AwaitGather, AwaitReply, Say, Done };

CStage g_cs = CStage::Arm;
bool g_life2 = false;
uint64_t g_csMs = 0, g_cNextMs = 0, g_nextSayMs = 0;
EL::ElementId g_ceid = EL::kInvalidId;
ue_wrap::CachedObjRef g_mirror;
int g_invBefore = 0;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
std::string g_sayLine;

void CEnter(CStage s) {
    g_cs = s;
    g_csMs = NowMs();
}

bool CExpired(uint64_t bound) { return NowMs() - g_csMs > bound; }

void Fail(const std::string& why) {
    UE_LOGW("%s c%d depart FAIL -- %s", kTag, kSlot, why.c_str());
    PAR::dev::HoldCommits(false);
    g_sayLine = DoneHead(kDepartRow) + "fail " + why.substr(0, 120);
    CEnter(CStage::Say);
}

void Resolve() {
    void* const mirror = EL::LivePropActor(g_ceid);
    if (!mirror) {
        if (CExpired(kMirrorBoundMs)) Fail("the host's prop never reached this client");
        return;
    }
    g_invBefore = InventoryCount(kDepartRow.leaf);
    if (g_invBefore < 0) {
        Fail("the inventory could not be read");
        return;
    }
    g_mirror.Set(mirror);
    // Life 1 holds its commit for good: the rig kills this process, and the host's reservation is what is left.
    if (!g_life2) PAR::dev::HoldCommits(true);
    FVector at{};
    if (!E::TryGetActorLocation(mirror, at)) {
        Fail("the mirror's place could not be read");
        return;
    }
    g_walk = coop::director::StartBackgroundWalk(at, kReachCm, kWalkDeadlineS);
    UE_LOGI("%s c%d depart life %d: walking to eid %u at (%.0f, %.0f, %.0f)", kTag, kSlot, g_life2 ? 2 : 1,
            static_cast<unsigned>(g_ceid), at.X, at.Y, at.Z);
    CEnter(CStage::Walk);
}

void Act(void* player) {
    void* const mirror = g_mirror.Get();
    if (!mirror) {
        Fail("the mirror was gone before the act");
        return;
    }
    if (const char* why = RunVerb(kDepartRow, player, mirror)) {
        Fail(why);
        return;
    }
    if (!g_mirror.Alive() || PAR::dev::PendingId() == 0) {
        Fail("the take was not cancelled at the call and asked");
        return;
    }
    UE_LOGI("%s c%d depart life %d: the call was cancelled and asked (request %u)", kTag, kSlot, g_life2 ? 2 : 1,
            static_cast<unsigned>(PAR::dev::PendingId()));
    CEnter(CStage::AwaitReply);
}

void ReadReply() {
    if (g_mirror.Alive()) {
        Fail(std::string("the mirror stayed after the host's grant") + MirrorState(g_mirror.Get()));
        return;
    }
    if (InventoryCount(kDepartRow.leaf) != g_invBefore + 1) {
        Fail("the inventory did not gain the crowbar");
        return;
    }
    if (!g_life2) {
        // The rig kills this process on the marker; the commit stays held, so the reservation is the host's to release.
        UE_LOGI("%s", LifeOneMarker().c_str());
        CEnter(CStage::Done);
        return;
    }
    UE_LOGI("%s c%d depart PASS -- the newcomer's honest take was granted and replayed", kTag, kSlot);
    g_sayLine = DoneHead(kDepartRow) + "ok";
    CEnter(CStage::Say);
}

void ClientStep(coop::net::Session* s, void* player, uint64_t now) {
    const bool poll = now >= g_cNextMs;
    if (poll) g_cNextMs = now + kPollEveryMs;
    switch (g_cs) {
    case CStage::Arm: {
        const uint8_t self = coop::players::Registry::Get().LocalPeerId();
        if (!OwnJoinOver() || !s->connected() || self == 0) return;
        if (self != kSlot) {
            UE_LOGI("%s c%u depart idle -- this phase runs on c%d only", kTag, static_cast<unsigned>(self), kSlot);
            CEnter(CStage::Done);
            return;
        }
        CEnter(CStage::Decide);
        return;
    }
    case CStage::Decide: {
        // The host's record is replayed to a joiner, so the first life's ready line reaches the newcomer as history.
        // Life 1 acts only on a ready line that arrived live; the newcomer waits for its own line. A life 1 that
        // first reads the ready line after the feed has retired it from the live tier (11 s) would wait for a line
        // that never comes, and the bound below FAILs it: loud, never a pass.
        if (!poll) return;
        std::string rest;
        if (HostSaid(NewcomerHead(), &rest)) {
            g_ceid = static_cast<EL::ElementId>(std::strtoul(rest.c_str(), nullptr, 10));
            g_life2 = true;
            CEnter(CStage::Forge);
            return;
        }
        if (HostSaidLive(ReadyHead(kDepartRow), &rest)) {
            g_ceid = static_cast<EL::ElementId>(std::strtoul(rest.c_str(), nullptr, 10));
            CEnter(CStage::Resolve);
            return;
        }
        if (CExpired(kReadyBoundMs)) Fail("neither a live ready line nor the newcomer line came from the host");
        return;
    }
    case CStage::Forge: {
        // The first life's reservation id, as the dead life held it: the host released it at the departure, so
        // this commit matches nothing.
        const coop::net::PropActCommitPayload p{kLife1RequestId, 1, 0, static_cast<uint32_t>(g_ceid)};
        if (!s->SendReliableToSlot(0, coop::net::ReliableKind::PropActCommit, &p, sizeof(p))) {
            Fail("the forged commit could not be sent");
            return;
        }
        UE_LOGI("%s c%d depart: the forged commit for eid %u was sent", kTag, kSlot, static_cast<unsigned>(g_ceid));
        CEnter(CStage::AwaitBad);
        return;
    }
    case CStage::AwaitBad:
        if (!poll) return;
        if (!HostSaid(BadSeenLine(), nullptr)) {
            if (CExpired(kReadyBoundMs)) Fail("the host never said it tallied the forged commit");
            return;
        }
        CEnter(CStage::Resolve);
        return;
    case CStage::Resolve:
        Resolve();
        return;
    case CStage::Walk: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) return;
        g_walk.reset();
        if (st == 2) {
            Fail("the walk to the mirror did not arrive");
            return;
        }
        CEnter(CStage::AwaitGather);
        return;
    }
    case CStage::AwaitGather:
        // A host mirror still driven is parked with physics off, and a take of it bails: act when a press could gather it.
        if (!poll) return;
        if (g_mirror.Get() && !MirrorGatherable(g_mirror.Get(), kDepartRow.verb)) {
            if (CExpired(kGatherBoundMs)) Fail("the mirror never became gatherable here (the host's drive did not end)");
            return;
        }
        Act(player);
        return;
    case CStage::AwaitReply:
        if (PAR::dev::PendingId() != 0) {
            if (CExpired(kReplyBoundMs)) Fail("the request was still pending");
            return;
        }
        ReadReply();
        return;
    case CStage::Say:
        // A client's chat passes the host's rate limit only at the drills' spacing; a refused line is lost for good.
        if (now < g_nextSayMs) return;
        Say(g_sayLine);
        g_nextSayMs = now + coop::dev::kTalkEveryMs;
        g_mirror.Reset();
        CEnter(CStage::Done);
        return;
    case CStage::Done:
        return;
    }
}

}  // namespace

void DepartHostTick(coop::net::Session* s) {
    if (g_hs == HStage::Done) return;
    const uint64_t now = NowMs();
    if (now < g_hNextMs) return;
    g_hNextMs = now + kPollEveryMs;
    HostStep(s);
}

void DepartClientTick(coop::net::Session* s) {
    if (g_cs == CStage::Done) return;
    void* const player = coop::players::Registry::Get().Local();
    if (!player) return;
    ClientStep(s, player, NowMs());
}

void DepartReset() {
    g_hs = HStage::Arm;
    g_hsMs = g_hNextMs = 0;
    g_target.Reset();
    g_eid = EL::kInvalidId;
    g_base = HBase{};
    g_cs = CStage::Arm;
    g_life2 = false;
    g_csMs = g_cNextMs = g_nextSayMs = 0;
    g_ceid = EL::kInvalidId;
    g_mirror.Reset();
    g_invBefore = 0;
    g_walk.reset();
    g_sayLine.clear();
}

}  // namespace coop::dev::prop_act_drill::detail
