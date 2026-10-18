// coop/dev/prop_act_drill_race_client.cpp -- the clients' half of the act drill's phase `race`: c1 the taker, c2 the
// second taker or the blocker, told apart by their slot. See coop/dev/prop_act_drill.h; the table and the host's half
// are in prop_act_drill_race.cpp.
//
//   c1 -- pockets or eats the target; in four steps its commit is held until the others are answered.
//   c2 -- pockets, writes its own grab, plants its hook or forges a remover request, as the step says.

#include "prop_act_drill_race.h"

#include "coop/dev/prop_act_drill.h"

#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat_pace.h"
#include "coop/dev/hook_plant.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_request.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

namespace coop::dev::prop_act_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace EL  = coop::element;
namespace PAR = coop::prop_act_request;
using ue_wrap::FVector;
using Reason = coop::net::PropActReason;

constexpr float kEyeLiftCm = 60.f;   // the forged request's origin, above c2's origin

enum class CStage : uint8_t { Arm, Begin, AwaitReady, Resolve, Walk, AwaitGather, AwaitLine, AwaitReply, AwaitRelease,
                              AwaitFanout, Hold, AwaitOver, Say, Done };
// What follows a said line: the next step (c1), c1's done line being awaited (c2), c1's ready line, or nothing.
enum class Then : uint8_t { Advance, Over, Ready, Stop };

CStage g_cs = CStage::Arm;
Then g_then = Then::Stop;
int g_slot = 0;
uint64_t g_csMs = 0, g_cNextMs = 0, g_nextSayMs = 0;
size_t g_crow = 0;
EL::ElementId g_ceid = EL::kInvalidId;
ue_wrap::CachedObjRef g_mirror, g_own, g_hook, g_root;
int g_invBefore = 0;
uint32_t g_refusedBase = 0;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
std::string g_sayLine, g_lineHead;
bool g_sayFailed = false;
bool g_gripping = false;   // c2, client hand: the grab of the mirror, written every tick

void CEnter(CStage s) {
    g_cs = s;
    g_csMs = NowMs();
}

bool CExpired(uint64_t bound) { return NowMs() - g_csMs > bound; }

// What c2 holds on the mirror is let go.
void LetGo(void* player) {
    if (g_gripping && player) E::WriteMainPlayerGrabbingPair(player, nullptr, nullptr);
    g_gripping = false;
    if (g_hook.Raw()) coop::dev::hook_plant::Release(player, g_hook.Get());
    g_hook.Reset();
}

void Fail(const RaceRow& r, const std::string& why) {
    UE_LOGW("%s c%d race FAIL -- %s: %s", kTag, g_slot, r.act.name, why.c_str());
    if (g_slot == kSlotC1) PAR::dev::HoldCommits(false);
    LetGo(coop::players::Registry::Get().Local());
    g_sayLine = DoneHead(r.act, g_slot) + "fail " + why.substr(0, 120);
    g_sayFailed = true;
    g_then = Then::Stop;
    CEnter(CStage::Say);
}

void SayThen(const std::string& line, Then then) {
    g_sayLine = line;
    g_sayFailed = false;
    g_then = then;
    CEnter(CStage::Say);
}

void Pass(const RaceRow& r, const char* what) {
    UE_LOGI("%s c%d race step %s PASS -- %s", kTag, g_slot, r.act.name, what);
    SayThen(DoneHead(r.act, g_slot) + "ok", g_slot == kSlotC1 ? Then::Advance : Then::Over);
}

void Advance() {
    g_mirror.Reset();
    g_own.Reset();
    g_lineHead.clear();
    g_ceid = EL::kInvalidId;
    if (++g_crow < kRaceRowCount) {
        CEnter(CStage::Begin);
        return;
    }
    UE_LOGI("%s c%d race PASS -- all %u steps held on this client", kTag, g_slot, static_cast<unsigned>(kRaceRowCount));
    CEnter(CStage::Done);
}

void BeginWalk(const RaceRow& r, void* mirror) {
    FVector at{};
    if (!E::TryGetActorLocation(mirror, at)) {
        Fail(r, "the mirror's place could not be read");
        return;
    }
    g_walk = coop::director::StartBackgroundWalk(at, kReachCm, kWalkDeadlineS);
    UE_LOGI("%s c%d race %s: walking to eid %u at (%.0f, %.0f, %.0f)", kTag, g_slot, r.act.name,
            static_cast<unsigned>(g_ceid), at.X, at.Y, at.Z);
    CEnter(CStage::Walk);
}

void AwaitLine(const std::string& head) {
    g_lineHead = head;
    CEnter(CStage::AwaitLine);
}

// The call of a take: it must be cancelled at the call and ask the host.
void Act(const RaceRow& r, void* player) {
    void* const mirror = g_mirror.Get();
    if (!mirror) {
        Fail(r, "the mirror was gone before the act");
        return;
    }
    if (g_slot == kSlotC1 && r.act.setup) {
        if (const char* why = r.act.setup(player, mirror)) {
            Fail(r, why);
            return;
        }
    }
    g_refusedBase = PAR::dev::RefusedCount(Reason::Held);
    if (const char* why = RunVerb(r.act, player, mirror)) {
        Fail(r, why);
        return;
    }
    if (!g_mirror.Alive()) {
        Fail(r, "the mirror was gone right after the call: the act was not cancelled at the call");
        return;
    }
    if (PAR::dev::PendingId() == 0) {
        Fail(r, "the call opened no request");
        return;
    }
    UE_LOGI("%s c%d race %s: the call was cancelled and asked (request %u)", kTag, g_slot, r.act.name,
            static_cast<unsigned>(PAR::dev::PendingId()));
    CEnter(CStage::AwaitReply);
}

// The request is answered and the replay, if any, has run: what this client's copies show.
void ReadReply(const RaceRow& r) {
    const uint32_t refused = Since(PAR::dev::RefusedCount(Reason::Held), g_refusedBase);
    const bool refusedRow = g_slot == kSlotC1 ? C1Refused(r.blocker) : r.blocker == Blocker::C2Pocket;
    if (refusedRow) {
        if (refused != 1) {
            Fail(r, "the host's Held refusal was counted " + std::to_string(refused) + " times, 1 expected");
            return;
        }
        if (!g_mirror.Alive() || !EL::LivePropActor(g_ceid)) {
            Fail(r, "the mirror is gone after a refused take");
            return;
        }
        if (InventoryCount(r.act.leaf) != g_invBefore) {
            Fail(r, "the inventory changed on a refused take");
            return;
        }
        if (g_slot == kSlotC2) CEnter(CStage::AwaitFanout);   // the mirror goes with c1's destroy
        else Pass(r, "asked, refused as held; the mirror and the inventory stayed");
        return;
    }
    if (refused != 0) {
        Fail(r, "the host refused a take it should have granted");
        return;
    }
    if (g_mirror.Alive()) {
        Fail(r, "the mirror stayed after the host's grant");
        return;
    }
    const bool keepsItem = g_slot == kSlotC2 || r.act.ends == Ends::Inventory;
    if (keepsItem && InventoryCount(r.act.leaf) != g_invBefore + 1) {
        Fail(r, "the inventory did not gain the item");
        return;
    }
    if (g_slot == kSlotC1 && C1Holds(r.blocker)) CEnter(CStage::AwaitRelease);
    else Pass(r, "asked, granted, replayed; the mirror is gone and the item is where the verb puts it");
}

// c2's request against c1's reservation: the payload as a tool gun's remover builds it.
void Forge(coop::net::Session* s, const RaceRow& r, void* player) {
    void* const mirror = g_mirror.Get();
    FVector from{}, at{};
    if (!mirror || !E::TryGetActorLocation(player, from) || !E::TryGetActorLocation(mirror, at)) {
        Fail(r, "the mirror's or the player's place could not be read");
        return;
    }
    coop::net::RemoverRequestPayload p{};
    p.elementId = static_cast<uint32_t>(g_ceid);
    p.origin[0] = from.X; p.origin[1] = from.Y; p.origin[2] = from.Z + kEyeLiftCm;
    p.hit[0] = at.X;      p.hit[1] = at.Y;      p.hit[2] = at.Z;
    if (!s->SendReliableToSlot(0, coop::net::ReliableKind::RemoverRequest, &p, sizeof(p))) {
        Fail(r, "the remover request could not be sent");
        return;
    }
    UE_LOGI("%s c%d race %s: the remover request for eid %u was sent", kTag, g_slot, r.act.name,
            static_cast<unsigned>(g_ceid));
    CEnter(CStage::AwaitOver);
}

// Where c1 and c2 begin a step, by what they do in it.
void Begin(const RaceRow& r, void* player) {
    g_mirror.Reset();
    if (g_slot == kSlotC1 && r.blocker == Blocker::C2Takes) {
        void* own = nullptr;
        if (const char* why = r.act.make(player, &own)) {
            Fail(r, why);
            return;
        }
        g_own.Set(own);
        UE_LOGI("%s c1 race %s: its own disc is spawned", kTag, r.act.name);
        SayThen(KeyHead() + Narrow(ue_wrap::prop::GetInteractableKeyString(own)), Then::Ready);
        return;
    }
    if (g_slot == kSlotC2 && (r.blocker == Blocker::HostActs || r.blocker == Blocker::HostGrab)) {
        CEnter(CStage::AwaitOver);   // c2 has no part in this step
        return;
    }
    CEnter(CStage::AwaitReady);
}

// The step's ready line is in: the mirror is here, then c1's held commits, c2's hold or hook and the walk begin.
void Resolve(const RaceRow& r, void* player) {
    void* const mirror = EL::LivePropActor(g_ceid);
    if (!mirror) {
        if (CExpired(kMirrorBoundMs)) Fail(r, "the host's prop never reached this client");
        return;
    }
    g_invBefore = InventoryCount(r.act.leaf);
    if (g_invBefore < 0) {
        Fail(r, "the inventory could not be read");
        return;
    }
    g_mirror.Set(mirror);
    if (g_slot == kSlotC1 && C1Holds(r.blocker)) PAR::dev::HoldCommits(true);
    if (g_slot == kSlotC2 && r.blocker == Blocker::D1Remove) {
        AwaitLine(HostLine(r, "c1 reserved"));
        return;
    }
    if (g_slot == kSlotC2 && r.blocker == Blocker::C2Grab) {
        g_root.Set(RootOf(mirror));
        g_gripping = true;
        CEnter(CStage::Hold);
        return;
    }
    if (g_slot == kSlotC2 && r.blocker == Blocker::C2Hook) {
        const char* note = "";
        void* const hook = coop::dev::hook_plant::Plant(player, mirror, &note);
        if (!hook) {
            Fail(r, std::string("c2's hook could not be planted: ") + note);
            return;
        }
        g_hook.Set(hook);
        CEnter(CStage::Hold);
        return;
    }
    BeginWalk(r, mirror);
}

// The walk is over: c1 waits for c2's hold to be read where the step needs it, c2 for c1's reservation.
void Arrived(const RaceRow& r, void* player) {
    if (g_slot == kSlotC1 && r.blocker == Blocker::C2Grab) AwaitLine(HostLine(r, "held"));
    else if (g_slot == kSlotC1 && r.blocker == Blocker::C2Hook) AwaitLine(HostLine(r, "hooked"));
    else if (g_slot == kSlotC2 && r.blocker == Blocker::C2Pocket) AwaitLine(HostLine(r, "c1 reserved"));
    else Act(r, player);
}

// c1's done line, as c2 reads it: it ends the step for c2. False while there is none yet.
bool C1Done(const RaceRow& r, bool* failed) {
    std::string rest;
    if (!HostSaid(DoneHead(r.act, kSlotC1), &rest)) return false;
    *failed = rest.compare(0, 4, "fail") == 0;
    return true;
}

void ClientStep(coop::net::Session* s, void* player, uint64_t now) {
    const RaceRow& r = kRaceRows[g_crow < kRaceRowCount ? g_crow : 0];
    const bool poll = now >= g_cNextMs;
    if (poll) g_cNextMs = now + kPollEveryMs;
    switch (g_cs) {
    case CStage::Arm: {
        const uint8_t self = coop::players::Registry::Get().LocalPeerId();
        if (!OwnJoinOver() || !s->connected() || self == 0) return;
        if (self != kSlotC1 && self != kSlotC2) {
            UE_LOGI("%s c%u race idle -- this phase runs on c1 and c2 only", kTag, static_cast<unsigned>(self));
            CEnter(CStage::Done);
            return;
        }
        g_slot = self;
        CEnter(CStage::Begin);
        return;
    }
    case CStage::Begin:
        Begin(r, player);
        return;
    case CStage::AwaitReady: {
        if (!poll) return;
        std::string rest;
        if (!HostSaid(ReadyHead(r.act), &rest)) {
            if (CExpired(kReadyBoundMs)) Fail(r, "the host never said the step was ready");
            return;
        }
        g_ceid = static_cast<EL::ElementId>(std::strtoul(rest.c_str(), nullptr, 10));
        CEnter(g_slot == kSlotC1 && r.blocker == Blocker::C2Takes ? CStage::AwaitFanout : CStage::Resolve);
        return;
    }
    case CStage::Resolve:
        Resolve(r, player);
        return;
    case CStage::Walk: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) return;
        g_walk.reset();
        if (st == 2) {
            Fail(r, "the walk to the mirror did not arrive");
            return;
        }
        CEnter(CStage::AwaitGather);
        return;
    }
    case CStage::AwaitGather: {
        // A granted take gathers its mirror, so it waits until the host's drive of it has ended: c1's take of a host mirror,
        // and c2's take of c1's own spawn. Every other act here meets a mirror somebody holds or hooks, or is c2's refused
        // take: held is the state it must meet.
        if (!poll) return;
        const bool granted = g_slot == kSlotC1 ? !C1Refused(r.blocker) && r.act.target != Target::Client
                                               : r.act.target == Target::Client;
        if (granted && g_mirror.Get() &&
            !MirrorGatherable(g_mirror.Get())) {
            if (CExpired(kGatherBoundMs))
                Fail(r, "the mirror never became gatherable here (the host's drive did not end)");
            return;
        }
        Arrived(r, player);
        return;
    }
    case CStage::AwaitLine:
        if (!poll) return;
        if (!HostSaid(g_lineHead, nullptr)) {
            if (CExpired(kReadyBoundMs)) Fail(r, "the host never said: " + g_lineHead);
            return;
        }
        if (g_slot == kSlotC2 && r.blocker == Blocker::D1Remove) Forge(s, r, player);
        else Act(r, player);
        return;
    case CStage::AwaitReply:
        if (PAR::dev::PendingId() != 0) {
            if (CExpired(kReplyBoundMs)) Fail(r, "the request was still pending");
            return;
        }
        ReadReply(r);
        return;
    case CStage::AwaitRelease:
        // c1's held commit goes once the host has answered the others.
        if (!poll) return;
        if (!HostSaid(HostLine(r, "answered"), nullptr)) {
            if (CExpired(kReadyBoundMs)) Fail(r, "the host never answered the other peer");
            return;
        }
        PAR::dev::ReleaseHeldCommits();
        PAR::dev::HoldCommits(false);
        Pass(r, "asked, granted, replayed; its commit was held until the others were answered, then sent");
        return;
    case CStage::AwaitFanout:
        if (g_slot == kSlotC1 ? !g_own.Alive() : (!g_mirror.Alive() && !EL::LivePropActor(g_ceid))) {
            Pass(r, g_slot == kSlotC1 ? "the host authored the disc and the destroy came back: this copy is gone"
                                      : "refused as held; the mirror went with c1's destroy");
            return;
        }
        if (CExpired(kDoneBoundMs)) Fail(r, "the destroy never reached this copy");
        return;
    case CStage::Hold: {
        // c2's grab is written every tick, its hook stands, until c1's act has ended.
        if (g_gripping) {
            if (void* const mirror = g_mirror.Get()) E::WriteMainPlayerGrabbingPair(player, mirror, g_root.Get());
        }
        if (!poll) return;
        bool failed = false;
        if (!C1Done(r, &failed)) {
            if (CExpired(kDoneBoundMs)) Fail(r, "c1 never ended its act");
            return;
        }
        LetGo(player);
        if (failed) CEnter(CStage::Done);
        else Advance();
        return;
    }
    case CStage::AwaitOver: {
        if (!poll) return;
        bool failed = false;
        if (!C1Done(r, &failed)) {
            if (CExpired(kDoneBoundMs)) Fail(r, "c1 never said it was done");
            return;
        }
        if (failed) CEnter(CStage::Done);
        else Advance();
        return;
    }
    case CStage::Say:
        // A client's chat passes the host's rate limit only at the drills' spacing; a refused line is lost for good.
        if (now < g_nextSayMs) return;
        Say(g_sayLine);
        g_nextSayMs = now + coop::dev::kTalkEveryMs;
        g_mirror.Reset();
        if (g_sayFailed || g_then == Then::Stop) CEnter(CStage::Done);
        else if (g_then == Then::Advance) Advance();
        else if (g_then == Then::Over) CEnter(CStage::AwaitOver);
        else CEnter(CStage::AwaitReady);
        return;
    case CStage::Done:
        return;
    }
}

}  // namespace

void RaceClientTick(coop::net::Session* s) {
    if (g_cs == CStage::Done) return;
    void* const player = coop::players::Registry::Get().Local();
    if (!player) return;
    ClientStep(s, player, NowMs());
}


void RaceClientReset() {
    g_cs = CStage::Arm;
    g_then = Then::Stop;
    g_slot = 0;
    g_csMs = g_cNextMs = g_nextSayMs = 0;
    g_crow = 0;
    g_ceid = EL::kInvalidId;
    LetGo(coop::players::Registry::Get().Local());
    g_mirror.Reset();
    g_own.Reset();
    g_root.Reset();
    g_invBefore = 0;
    g_refusedBase = 0;
    g_walk.reset();
    g_sayLine.clear();
    g_lineHead.clear();
    g_sayFailed = false;
}

}  // namespace coop::dev::prop_act_drill::detail
