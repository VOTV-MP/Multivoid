// coop/dev/prop_act_drill_edge.cpp -- the step table and the host's half of the act drill's phase `edge`: the windows
// of the ask. A request that arrives before the previous commit, a target that dies at the reply or before the commit,
// a take during the join window, a second act in the tick of the first, and a grant that comes after the client gave
// up. See coop/dev/prop_act_drill.h; c1's half is in prop_act_drill_edge_client.cpp.
//
//   HOST -- spawns each step's target, says a ready line, kills the target where the step needs it dead (the host's
//           own K2_DestroyActor, not a player verb), says a line at each point c1 waits on, and judges its own
//           counters once c1 said its done line. It asserts counters and outcomes, never a refusal line.

#include "prop_act_drill_edge.h"

#include "coop/dev/prop_act_drill.h"

#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_host.h"
#include "coop/props/prop_hold.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cstdint>
#include <string>

namespace coop::dev::prop_act_drill::detail {

// The window row runs first (see Kind). Its Row carries only the name: it has no target and no verb the core runs.
const EdgeRow kEdgeRows[] = {
    {{"join window",           nullptr, L"",               Verb::Pocket, Ends::Stays,     nullptr, Target::Client},   Kind::Window},
    {{"request before commit", kCrowbar, L"prop_crowbar_C", Verb::Pocket, Ends::Inventory, nullptr, Target::HostTwo}, Kind::Before},
    {{"dead at the reply",     kCrowbar, L"prop_crowbar_C", Verb::Pocket, Ends::Stays,     nullptr},                  Kind::Dead},
    {{"target gone",           kCrowbar, L"prop_crowbar_C", Verb::Pocket, Ends::Inventory, nullptr},                  Kind::Gone},
    {{"second act",            kCrowbar, L"prop_crowbar_C", Verb::Pocket, Ends::Inventory, nullptr, Target::HostTwo}, Kind::Second},
    {{"late grant",            kCrowbar, L"prop_crowbar_C", Verb::Pocket, Ends::Inventory, nullptr, Target::HostTwo}, Kind::Late},
};
const size_t kEdgeRowCount = sizeof(kEdgeRows) / sizeof(kEdgeRows[0]);

namespace {

namespace E   = ue_wrap::engine;
namespace EL  = coop::element;
namespace PAH = coop::prop_act_host;
using Tally = PAH::Tally;
using CommitStep = PAH::internal::CommitStep;

constexpr int kSlot = kClientSlot;

unsigned Wire(EL::ElementId eid) { return eid == EL::kInvalidId ? 0u : static_cast<unsigned>(eid); }

// The host's counters as the step begins.
struct EBase {
    uint32_t destroy = 0, abort = 0, answered = 0, waits = 0, targetGone = 0, destroyFailed = 0;
};

enum class HStage : uint8_t { Arm, Spawn, Announce, Act, AwaitDone, Judge, Done };
HStage g_hs = HStage::Arm;
uint64_t g_hsMs = 0, g_hNextMs = 0;
size_t g_hrow = 0;
ue_wrap::CachedObjRef g_target, g_target2;
EL::ElementId g_eid = EL::kInvalidId, g_eid2 = EL::kInvalidId;
EBase g_base;
bool g_baseTaken = false;       // the window row's counters were taken (see HStage::Arm)
uint32_t g_phaseAnswered = 0;   // the host's answered total for the slot at that moment: the whole phase's baseline

void HEnter(HStage s) {
    g_hs = s;
    g_hsMs = NowMs();
}

bool HExpired(uint64_t bound) { return NowMs() - g_hsMs > bound; }

void HostFail(const EdgeRow* row, const std::string& why) {
    UE_LOGW("%s host %s FAIL -- %s%s%s", kTag, PhaseName().c_str(), row ? row->act.name : "", row ? ": " : "", why.c_str());
    g_hs = HStage::Done;
}

EBase TakeBase() {
    EBase b;
    b.destroy = PAH::CommitsOf(kSlot, CommitStep::Destroy);
    b.abort = PAH::CommitsOf(kSlot, CommitStep::Abort);
    b.answered = PAH::AnsweredTotal(kSlot);
    b.waits = PAH::WaitsOf(kSlot, PAH::WaitCause::Reservation);
    b.targetGone = PAH::TallyOf(Tally::TargetGone);
    b.destroyFailed = PAH::TallyOf(Tally::DestroyFailed);
    return b;
}

// What a step's requests are answered with, as its judge asserts: the window none, a request before the commit two, a
// dead target's grant one, a gone target's grant one, the second act one, a late grant two.
uint32_t AnswersOf(Kind k) {
    switch (k) {
    case Kind::Window: return 0;
    case Kind::Before: return 2;
    case Kind::Dead:   return 1;
    case Kind::Gone:   return 1;
    case Kind::Second: return 1;
    case Kind::Late:   return 2;
    }
    return 0;
}

uint32_t ExpectedAnswers() {
    uint32_t sum = 0;
    for (size_t i = 0; i < kEdgeRowCount; ++i) sum += AnswersOf(kEdgeRows[i].kind);
    return sum;
}

void ResetStep() {
    g_target.Reset();
    g_target2.Reset();
    g_eid = g_eid2 = EL::kInvalidId;
}

// A target is announced: its eid set. False while it has not been, and after a FAIL.
bool Announced(const EdgeRow& r, ue_wrap::CachedObjRef& ref, EL::ElementId* eid) {
    void* const actor = ref.Get();
    if (!actor) {
        HostFail(&r, "the spawned prop was gone before it was announced");
        return false;
    }
    *eid = EL::Registry::Get().EidForActor(actor);
    if (*eid != EL::kInvalidId) return true;
    if (HExpired(kAnnounceBoundMs)) HostFail(&r, "the spawned prop was never announced");
    return false;
}

// c1 said its step failed: the phase ends there rather than on a bound. True after a FAIL.
bool ClientFailed(const EdgeRow& r) {
    std::string rest;
    if (!ClientSaid(kSlot, DoneHead(r.act), &rest) || rest == "ok") return false;
    HostFail(&r, "c1 reported: " + rest);
    return true;
}

// The targets are announced: take the counters and say the step is ready.
void SayReady(const EdgeRow& r) {
    g_base = TakeBase();
    std::string line = ReadyHead(r.act) + std::to_string(Wire(g_eid));
    if (g_eid2 != EL::kInvalidId) line += " eid2=" + std::to_string(Wire(g_eid2));
    Say(line);
    UE_LOGI("%s host %s said ready: eid %u", kTag, r.act.name, Wire(g_eid));
    HEnter(r.kind == Kind::Before || r.kind == Kind::Dead || r.kind == Kind::Gone ? HStage::Act : HStage::AwaitDone);
}

// The host's own K2_DestroyActor of the step's target. False after a FAIL.
bool KillTarget(const EdgeRow& r) {
    void* const actor = g_target.Get();
    if (!actor || !E::DestroyActor(actor)) {
        HostFail(&r, "the host could not destroy the target");
        return false;
    }
    UE_LOGI("%s host %s: the target was destroyed on the host", kTag, r.act.name);
    return true;
}

// The point of the step that only the host can supply: c1's reservation waiting, a kill at the reply, a kill between
// the replay and the commit.
void Act(const EdgeRow& r) {
    if (ClientFailed(r)) return;
    switch (r.kind) {
    case Kind::Before:
        // The second request waits behind the first's reservation, which holds until the first commit is released.
        if (PAH::WaitsOf(kSlot, PAH::WaitCause::Reservation) <= g_base.waits) {
            if (HExpired(kDoneBoundMs)) HostFail(&r, "the second request never waited on the reservation");
            return;
        }
        if (Since(PAH::AnsweredTotal(kSlot), g_base.answered) != 1 ||
            PAH::CommitsOf(kSlot, CommitStep::Destroy) != g_base.destroy) {
            HostFail(&r, "the second request was answered, or the first commit arrived, before the wait was seen");
            return;
        }
        Say(HostLine(r, "waiting"));
        HEnter(HStage::AwaitDone);
        return;
    case Kind::Dead:
        if (coop::prop_hold::ReservedEid(kSlot) != static_cast<uint32_t>(g_eid)) {
            if (HExpired(kDoneBoundMs)) HostFail(&r, "c1 never reserved the target");
            return;
        }
        if (!KillTarget(r)) return;
        HEnter(HStage::AwaitDone);
        return;
    case Kind::Gone:
        if (!ClientSaid(kSlot, ClientLine(r, "committed-held"), nullptr)) {
            if (HExpired(kDoneBoundMs)) HostFail(&r, "c1 never said its commit was held");
            return;
        }
        if (coop::prop_hold::ReservedEid(kSlot) != static_cast<uint32_t>(g_eid)) {
            HostFail(&r, "c1 held its commit but the reservation is not standing");
            return;
        }
        if (!KillTarget(r)) return;
        Say(HostLine(r, "destroyed"));
        HEnter(HStage::AwaitDone);
        return;
    default:
        return;
    }
}

// The step's counters, once c1 said it was done. Wait is called again until the bound.
Judged Judge(const EdgeRow& r, std::string* why) {
    const EBase& b = g_base;
    const uint32_t destroys = Since(PAH::CommitsOf(kSlot, CommitStep::Destroy), b.destroy);
    const uint32_t aborts = Since(PAH::CommitsOf(kSlot, CommitStep::Abort), b.abort);
    const uint32_t answered = Since(PAH::AnsweredTotal(kSlot), b.answered);
    const bool reserved = coop::prop_hold::ReservationCount(kSlot) != 0;
    const bool live1 = EL::LivePropActor(g_eid) != nullptr;
    const bool live2 = g_eid2 != EL::kInvalidId && EL::LivePropActor(g_eid2) != nullptr;
    switch (r.kind) {
    case Kind::Window:
        if (answered != 0) {
            *why = "the host answered " + std::to_string(answered) + " requests from the window";
            return Judged::Fail;
        }
        *why = "no request from the window reached the host";
        return Judged::Pass;
    case Kind::Before:
        if (destroys < 2 || answered < 2 || live1 || live2 || reserved) {
            *why = std::to_string(destroys) + " destroy commits, " + std::to_string(answered) + " answers";
            return destroys > 2 || answered > 2 ? Judged::Fail : Judged::Wait;
        }
        if (destroys != 2 || answered != 2) {
            *why = std::to_string(destroys) + " destroy commits and " + std::to_string(answered) + " answers for two takes";
            return Judged::Fail;
        }
        *why = "the second request waited, was answered after the first commit, and both takes committed";
        return Judged::Pass;
    case Kind::Dead:
        if (aborts == 0 || reserved) {
            *why = "no abort commit yet, or the reservation still stands";
            return Judged::Wait;
        }
        if (aborts != 1 || destroys != 0 || PAH::TallyOf(Tally::TargetGone) != b.targetGone || live1) {
            *why = std::to_string(aborts) + " aborts, " + std::to_string(destroys) + " destroys, target-gone moved or the eid is live";
            return Judged::Fail;
        }
        *why = "the client aborted a dead target's grant: one abort, the reservation back to 0, target-gone unchanged";
        return Judged::Pass;
    case Kind::Gone:
        if (PAH::TallyOf(Tally::TargetGone) == b.targetGone || reserved) {
            *why = "target-gone not tallied yet, or the reservation still stands";
            return Judged::Wait;
        }
        if (PAH::TallyOf(Tally::TargetGone) != b.targetGone + 1 || PAH::TallyOf(Tally::DestroyFailed) != b.destroyFailed ||
            destroys != 0) {
            *why = "target-gone moved by more than 1, destroy-failed moved or a destroy commit was taken";
            return Judged::Fail;
        }
        *why = "the commit found the target gone: target-gone +1, destroy-failed unchanged, the reservation back to 0";
        return Judged::Pass;
    case Kind::Second:
        if (destroys == 0 || answered == 0) {
            *why = "no destroy commit or answer yet";
            return Judged::Wait;
        }
        if (answered != 1 || destroys != 1 || live1 || !live2 || reserved) {
            *why = std::to_string(answered) + " answers and " + std::to_string(destroys) +
                   " destroys for one sent request, or the first stands, the second is gone or a reservation stands";
            return Judged::Fail;
        }
        *why = "only the first act was sent: answered +1, one destroy, the second prop alive";
        return Judged::Pass;
    case Kind::Late:
        if (answered < 2 || destroys == 0 || aborts == 0 || reserved) {
            *why = std::to_string(answered) + " answers, " + std::to_string(destroys) + " destroys, " +
                   std::to_string(aborts) + " aborts";
            return Judged::Wait;
        }
        if (answered != 2 || destroys != 1 || aborts != 1 || live1 || !live2) {
            *why = std::to_string(answered) + " answers, " + std::to_string(destroys) + " destroys, " +
                   std::to_string(aborts) + " aborts, or the first stands or the second is gone";
            return Judged::Fail;
        }
        *why = "the late grant was answered with an abort: the second granted after the release, then one abort, the reservation back to 0, the second prop alive";
        return Judged::Pass;
    }
    return Judged::Fail;
}

void HostStep(coop::net::Session* s) {
    const EdgeRow& r = kEdgeRows[g_hrow < kEdgeRowCount ? g_hrow : 0];
    switch (g_hs) {
    case HStage::Arm:
        // The window row's baseline is the counters at the first tick the host sees the slot joined and not yet
        // world-ready, as the hostile not-yet arm takes it: an answer to a request sent in the window is then counted.
        if (!g_baseTaken) {
            if (!s->HasPeerConn(kSlot)) return;
            if (s->IsSlotWorldReady(kSlot)) {
                HostFail(nullptr, "the slot was world-ready before the host saw it joined");
                return;
            }
            g_base = TakeBase();
            g_phaseAnswered = g_base.answered;
            g_baseTaken = true;
        }
        // c1's announce is held by its window row, so this waits for the window row's end: the join and its late
        // window are then over, and what is spawned reaches the client as a live spawn.
        if (!s->IsSlotWorldReady(kSlot) || !coop::prop_snapshot::IsBracketClosed(kSlot) ||
            coop::join_window_baseline::IsLateWindowOpen(kSlot) || !coop::players::Registry::Get().Local())
            return;
        UE_LOGI("%s host edge armed on slot %d", kTag, kSlot);
        HEnter(HStage::AwaitDone);
        return;
    case HStage::Spawn: {
        void* actor = nullptr;
        if (const char* why = SpawnTarget(r.act, 0.f, &actor)) {
            HostFail(&r, why);
            return;
        }
        g_target.Set(actor);
        if (TwoProps(r.kind)) {
            void* second = nullptr;
            if (const char* why = SpawnTarget(r.act, kPairGapCm, &second)) {
                HostFail(&r, why);
                return;
            }
            g_target2.Set(second);
        }
        HEnter(HStage::Announce);
        return;
    }
    case HStage::Announce:
        if (!Announced(r, g_target, &g_eid)) return;
        if (TwoProps(r.kind) && !Announced(r, g_target2, &g_eid2)) return;
        SayReady(r);
        return;
    case HStage::Act:
        Act(r);
        return;
    case HStage::AwaitDone: {
        std::string rest;
        if (!ClientSaid(kSlot, DoneHead(r.act), &rest)) {
            if (HExpired(kDoneBoundMs)) HostFail(&r, "c1 never said it was done");
            return;
        }
        if (rest != "ok") {
            HostFail(&r, "c1 reported: " + rest);
            return;
        }
        HEnter(HStage::Judge);
        return;
    }
    case HStage::Judge: {
        std::string why;
        switch (Judge(r, &why)) {
        case Judged::Wait:
            if (HExpired(kJudgeBoundMs)) HostFail(&r, "still waiting: " + why);
            return;
        case Judged::Fail:
            HostFail(&r, why);
            return;
        case Judged::Pass:
            UE_LOGI("%s host edge step %s PASS -- %s", kTag, r.act.name, why.c_str());
            ResetStep();
            if (++g_hrow < kEdgeRowCount) {
                HEnter(HStage::Spawn);
                return;
            }
            // The whole phase's answers, from the first sight of the join: a request answered that no step sent (the
            // second act's red) is a FAIL. No settle poll: the late-grant step spans more than the client's 10 s
            // expiry, so it outlasts any answer the second act's red could draw.
            if (const uint32_t sum = ExpectedAnswers(), got = Since(PAH::AnsweredTotal(kSlot), g_phaseAnswered); got != sum) {
                HostFail(nullptr, "the host answered " + std::to_string(got) + " requests over the phase, its steps expect " +
                                      std::to_string(sum));
                return;
            }
            UE_LOGI("%s host edge PASS -- all %u steps held", kTag, static_cast<unsigned>(kEdgeRowCount));
            g_hs = HStage::Done;
            return;
        }
        return;
    }
    case HStage::Done:
        return;
    }
}

}  // namespace

void EdgeHostTick(coop::net::Session* s) {
    if (g_hs == HStage::Done) return;
    const uint64_t now = NowMs();
    if (now < g_hNextMs) return;
    g_hNextMs = now + kPollEveryMs;
    if (g_hs != HStage::Arm && !s->HasPeerConn(kSlot)) {
        HostFail(nullptr, "the client left before the verdict");
        return;
    }
    HostStep(s);
}

void EdgeReset() {
    g_hs = HStage::Arm;
    g_hsMs = g_hNextMs = 0;
    g_hrow = 0;
    ResetStep();
    g_base = EBase{};
    g_baseTaken = false;
    g_phaseAnswered = 0;
    EdgeClientReset();
}

}  // namespace coop::dev::prop_act_drill::detail
