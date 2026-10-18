// coop/dev/prop_act_drill_race.cpp -- the step table and the host's half of the act drill's phase `race`: two clients
// and the host contend for one shared prop, and the host judges that exactly one take went through and every other was
// refused. See coop/dev/prop_act_drill.h. Run with --observer: c1 and c2 are the first and second client, told apart by
// their slot; their half is in prop_act_drill_race_client.cpp.
//
//   HOST -- spawns each step's target (c1 spawns the last one), says a ready line, then says a line at each point the
//           clients wait on (c1's reservation held, the blocker answered, a hold or hook seen), and judges its own
//           counters once the clients said their done lines.

#include "prop_act_drill_race.h"

#include "coop/dev/prop_act_drill.h"
#include "coop/dev/drill_props.h"

#include "coop/element/registry.h"
#include "coop/items/hook_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_host.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/prop_hold.h"
#include "coop/props/prop_snapshot.h"
#include "coop/props/remote_prop.h"
#include "coop/props/remover_request.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <cstdint>
#include <string>

namespace coop::dev::prop_act_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace EL  = coop::element;
namespace PAH = coop::prop_act_host;
namespace PAR = coop::prop_act_request;
namespace RR  = coop::remover_request;
namespace DP  = coop::dev::drill_props;
using ue_wrap::FVector;
using Reason = coop::net::PropActReason;

// A whitelisted fresh birth: a client's own spawn of it is authored by the host (prop_drop_intent.cpp).
constexpr const wchar_t* kDisc = L"prop_floppyDisc_R_C";
constexpr float kOwnLiftCm = 60.f;   // a client's own spawn, above its player's origin
constexpr uint32_t kC2HookBit = 1u << kSlotC2;

// A client's own floppy disc, spawned above its player: keyed (its init mints the key) and without a row, which the
// gate reads as the client's own. The cell is read back first, else the row proves nothing.
const char* MakeOwnDisc(void* player, void** out) {
    void* const cls = R::FindClass(kDisc);
    if (!cls) return "class not loaded";
    FVector at{};
    if (!E::TryGetActorLocation(player, at)) return "the player's place is unread";
    void* const own = E::SpawnActor(cls, {at.X, at.Y, at.Z + kOwnLiftCm});
    if (!own) return "this client's own disc did not spawn";
    if (!DP::HasKey(own)) return "cell not reached: the spawn has no key";
    if (EL::Registry::Get().EidForActor(own) != EL::kInvalidId) return "cell not reached: the spawn already has a row";
    *out = own;
    return nullptr;
}

}  // namespace

const RaceRow kRaceRows[] = {
    {{"double",         kCrowbar, L"prop_crowbar_C", Verb::Pocket, Ends::Inventory, nullptr},      Blocker::C2Pocket},
    {{"host race",      kCrowbar, L"prop_crowbar_C", Verb::Pocket, Ends::Inventory, nullptr},      Blocker::HostActs},
    {{"host use race",  kFood,    L"prop_food_C",    Verb::Use,    Ends::Gone,      &SetLastUse},  Blocker::HostActs},
    {{"client hand",    kCrowbar, L"prop_crowbar_C", Verb::Pocket, Ends::Stays,     nullptr},      Blocker::C2Grab},
    {{"client hook",    kCrowbar, L"prop_crowbar_C", Verb::Pocket, Ends::Stays,     nullptr},      Blocker::C2Hook},
    {{"host held",      kCrowbar, L"prop_crowbar_C", Verb::Pocket, Ends::Stays,     nullptr},      Blocker::HostGrab},
    {{"d1 reservation", kCrowbar, L"prop_crowbar_C", Verb::Pocket, Ends::Inventory, nullptr},      Blocker::D1Remove},
    {{"other spawn",    nullptr,  kDisc,             Verb::Pocket, Ends::Gone,      nullptr, Target::Client, true, false, &MakeOwnDisc},
     Blocker::C2Takes},
};
const size_t kRaceRowCount = sizeof(kRaceRows) / sizeof(kRaceRows[0]);

namespace {

unsigned Wire(EL::ElementId eid) { return eid == EL::kInvalidId ? 0u : static_cast<unsigned>(eid); }

// ---- host (judge) ------------------------------------------------------------------------------------------------

// The host's counters as the step begins.
struct RaceBase {
    uint32_t destroy1 = 0, destroy2 = 0, abort2 = 0, held1 = 0, held2 = 0, targetGone = 0, cancels = 0, d1Held2 = 0,
             d1Removed2 = 0;
};

enum class HStage : uint8_t { Arm, Spawn, Announce, Grab, AwaitKey, AwaitReserved, Blocking, AwaitHeld, AwaitDone, Judge,
                              Done };
HStage g_hs = HStage::Arm;
uint64_t g_hsMs = 0, g_hNextMs = 0;
size_t g_hrow = 0;
ue_wrap::CachedObjRef g_target, g_hostRoot;
EL::ElementId g_eid = EL::kInvalidId;
std::string g_key;          // c1's disc key, once it said it
bool g_grabbing = false;    // host held: the host's own grab of the target, written every tick
RaceBase g_base;

void HEnter(HStage s) {
    g_hs = s;
    g_hsMs = NowMs();
}

bool HExpired(uint64_t bound) { return NowMs() - g_hsMs > bound; }

void ClearGrab() {
    if (!g_grabbing) return;
    g_grabbing = false;
    if (void* const p = coop::players::Registry::Get().Local()) E::WriteMainPlayerGrabbingPair(p, nullptr, nullptr);
}

void HostFail(const RaceRow* row, const std::string& why) {
    ClearGrab();
    UE_LOGW("%s host %s FAIL -- %s%s%s", kTag, PhaseName().c_str(), row ? row->act.name : "", row ? ": " : "", why.c_str());
    g_hs = HStage::Done;
}

// The host's own grab, re-written every tick (a cold-written grab can be cleared by the Blueprint).
void KeepGrab() {
    if (!g_grabbing) return;
    void* const actor = g_target.Get();
    void* const player = coop::players::Registry::Get().Local();
    if (actor && player) E::WriteMainPlayerGrabbingPair(player, actor, g_hostRoot.Get());
}

RaceBase TakeBase() {
    RaceBase b;
    b.destroy1 = PAH::CommitsOf(kSlotC1, PAH::internal::CommitStep::Destroy);
    b.destroy2 = PAH::CommitsOf(kSlotC2, PAH::internal::CommitStep::Destroy);
    b.abort2 = PAH::CommitsOf(kSlotC2, PAH::internal::CommitStep::Abort);
    b.held1 = PAH::AnsweredFor(kSlotC1, Reason::Held);
    b.held2 = PAH::AnsweredFor(kSlotC2, Reason::Held);
    b.targetGone = PAH::TallyOf(PAH::Tally::TargetGone);
    b.cancels = PAR::dev::HostCancels();
    b.d1Held2 = RR::AnsweredFor(kSlotC2, coop::net::RemoverReason::Held);
    b.d1Removed2 = RR::AnsweredFor(kSlotC2, coop::net::RemoverReason::Removed);
    return b;
}

struct KeyScan {
    const std::string* key;
    void* found;
};

// The host's copy of c1's disc: the instance carrying c1's key that has a row.
void* FindKeyed(const std::string& key) {
    void* const cls = ue_wrap::object_index::ClassByName(kDisc);
    if (!cls) return nullptr;
    KeyScan ctx{&key, nullptr};
    ue_wrap::object_index::ForEachInstance(
        cls,
        [](void* p, void* obj, int32_t) {
            auto* c = static_cast<KeyScan*>(p);
            if (!c->found && EL::Registry::Get().EidForActor(obj) != EL::kInvalidId &&
                Narrow(ue_wrap::prop::GetInteractableKeyString(obj)) == *c->key)
                c->found = obj;
        },
        &ctx);
    return ctx.found;
}

void ResetStep() {
    ClearGrab();
    g_target.Reset();
    g_hostRoot.Reset();
    g_eid = EL::kInvalidId;
    g_key.clear();
}

// The target is announced and the step's own setup is done: take the counters and say the step is ready.
void SayReady(const RaceRow& r) {
    g_base = TakeBase();
    Say(ReadyHead(r.act) + std::to_string(Wire(g_eid)));
    UE_LOGI("%s host %s said ready: eid %u", kTag, r.act.name, Wire(g_eid));
    if (C1Holds(r.blocker)) HEnter(HStage::AwaitReserved);
    else if (r.blocker == Blocker::C2Grab || r.blocker == Blocker::C2Hook) HEnter(HStage::AwaitHeld);
    else HEnter(HStage::AwaitDone);
}

// The blocker's move once c1 holds the reservation. True when it has been answered; the host's own move is made here,
// once, and a FAIL ends the phase.
bool Block(const RaceRow& r) {
    switch (r.blocker) {
    case Blocker::C2Pocket:
        return PAH::AnsweredFor(kSlotC2, Reason::Held) > g_base.held2;
    case Blocker::D1Remove:
        return RR::AnsweredFor(kSlotC2, coop::net::RemoverReason::Held) > g_base.d1Held2;
    case Blocker::HostActs: {
        void* const actor = g_target.Get();
        void* const player = coop::players::Registry::Get().Local();
        if (!actor || !player) {
            HostFail(&r, "the target or the host's player was gone before the host's own call");
            return false;
        }
        if (const char* why = RunVerb(r.act, player, actor)) {
            HostFail(&r, why);
            return false;
        }
        if (PAR::dev::HostCancels() != g_base.cancels + 1 || !g_target.Alive() || !EL::LivePropActor(g_eid)) {
            HostFail(&r, "the host's own call was not cancelled: the prop is gone or the cancel was not counted");
            return false;
        }
        return true;
    }
    default:
        return false;
    }
}

Judged C1Destroyed(uint32_t destroys, EL::ElementId eid, std::string* why) {
    if (destroys == 0) {
        *why = "no destroy commit from c1 yet";
        return Judged::Wait;
    }
    if (destroys != 1) {
        *why = std::to_string(destroys) + " destroy commits from c1 for one take";
        return Judged::Fail;
    }
    if (EL::LivePropActor(eid)) {
        *why = "the eid is still live on the host after the commit";
        return Judged::Fail;
    }
    return Judged::Pass;
}

Judged Judge(const RaceRow& r, EL::ElementId eid, std::string* why) {
    const RaceBase& b = g_base;
    const uint32_t d1 = Since(PAH::CommitsOf(kSlotC1, PAH::internal::CommitStep::Destroy), b.destroy1);
    const uint32_t d2 = Since(PAH::CommitsOf(kSlotC2, PAH::internal::CommitStep::Destroy), b.destroy2);
    const uint32_t a2 = Since(PAH::CommitsOf(kSlotC2, PAH::internal::CommitStep::Abort), b.abort2);
    const uint32_t held1 = Since(PAH::AnsweredFor(kSlotC1, Reason::Held), b.held1);
    const uint32_t held2 = Since(PAH::AnsweredFor(kSlotC2, Reason::Held), b.held2);
    Judged j = Judged::Pass;
    switch (r.blocker) {
    case Blocker::C2Pocket:
        if (held2 != 1 || d2 != 0 || a2 != 0) {
            *why = "c2 was answered Held " + std::to_string(held2) + " times and committed " + std::to_string(d2) +
                   " destroys, " + std::to_string(a2) + " aborts";
            return Judged::Fail;
        }
        j = C1Destroyed(d1, eid, why);
        if (j == Judged::Pass) *why = "c2 refused Held once and committed nothing; one destroy commit (c1's), the eid dead";
        return j;
    case Blocker::HostActs:
        if (PAR::dev::HostCancels() != b.cancels + 1 || PAH::TallyOf(PAH::Tally::TargetGone) != b.targetGone) {
            *why = "the host's call was not the one cancel, or c1's commit found the target gone";
            return Judged::Fail;
        }
        j = C1Destroyed(d1, eid, why);
        if (j == Judged::Pass) *why = "the host's call cancelled and the prop kept; c1's commit destroyed it, target-gone unchanged";
        return j;
    case Blocker::D1Remove:
        if (RR::AnsweredFor(kSlotC2, coop::net::RemoverReason::Removed) != b.d1Removed2) {
            *why = "D1 removed the reserved prop for c2";
            return Judged::Fail;
        }
        j = C1Destroyed(d1, eid, why);
        if (j == Judged::Pass) *why = "D1 answered c2 Held, not Removed; one destroy commit (c1's), the eid dead";
        return j;
    case Blocker::C2Grab:
    case Blocker::C2Hook:
    case Blocker::HostGrab:
        if (held1 != 1 || d1 != 0 || !EL::LivePropActor(eid)) {
            *why = "c1 was answered Held " + std::to_string(held1) + " times, committed " + std::to_string(d1) +
                   " destroys, or the eid is gone";
            return Judged::Fail;
        }
        *why = "c1 refused Held once, no destroy commit, the eid alive";
        return Judged::Pass;
    case Blocker::C2Takes:
        if (d2 == 0) {
            *why = "no destroy commit from c2 yet";
            return Judged::Wait;
        }
        if (d2 != 1 || d1 != 0 || EL::LivePropActor(eid)) {
            *why = std::to_string(d2) + " destroy commits from c2, " + std::to_string(d1) + " from c1, or the eid is live";
            return Judged::Fail;
        }
        *why = "one destroy commit from c2, none from c1, the eid dead";
        return Judged::Pass;
    }
    return Judged::Fail;
}

// Whether c<slot> said its done line; a done line that is not "ok" ends the phase.
bool DoneOk(const RaceRow& r, int slot, bool* failed) {
    std::string rest;
    if (!ClientSaid(slot, DoneHead(r.act, slot), &rest)) return false;
    if (rest != "ok") {
        HostFail(&r, "c" + std::to_string(slot) + " reported: " + rest);
        *failed = true;
        return false;
    }
    return true;
}

void HostStep(coop::net::Session* s) {
    const RaceRow& r = kRaceRows[g_hrow < kRaceRowCount ? g_hrow : 0];
    switch (g_hs) {
    case HStage::Arm:
        // Both joins and their late windows are over, so what is spawned reaches the clients as a live spawn.
        for (const int slot : {kSlotC1, kSlotC2})
            if (!s->IsSlotWorldReady(slot) || !coop::prop_snapshot::IsBracketClosed(slot) ||
                coop::join_window_baseline::IsLateWindowOpen(slot))
                return;
        if (!coop::players::Registry::Get().Local() || !ClientPuppetReady()) return;
        UE_LOGI("%s host race armed on slots %d and %d", kTag, kSlotC1, kSlotC2);
        HEnter(r.act.target == Target::Client ? HStage::AwaitKey : HStage::Spawn);
        return;
    case HStage::Spawn: {
        void* actor = nullptr;
        if (const char* why = SpawnTarget(r.act, 0.f, &actor)) {
            HostFail(&r, why);
            return;
        }
        g_target.Set(actor);
        HEnter(HStage::Announce);
        return;
    }
    case HStage::Announce: {
        void* const actor = g_target.Get();
        if (!actor) {
            HostFail(&r, "the spawned prop was gone before it was announced");
            return;
        }
        g_eid = EL::Registry::Get().EidForActor(actor);
        if (g_eid == EL::kInvalidId) {
            if (HExpired(kAnnounceBoundMs)) HostFail(&r, "the spawned prop was never announced");
            return;
        }
        if (r.blocker != Blocker::HostGrab) {
            SayReady(r);
            return;
        }
        // The host's own grab stands before the step is said ready, so the ready line means the prop is held.
        g_hostRoot.Set(RootOf(actor));
        g_grabbing = true;
        HEnter(HStage::Grab);
        return;
    }
    case HStage::Grab:
        if (!coop::prop_hold::HostHolds(g_target.Get())) {
            if (HExpired(kAnnounceBoundMs)) HostFail(&r, "the host's own grab was never read as a hold");
            return;
        }
        SayReady(r);
        return;
    case HStage::AwaitKey:
        // c1 spawns the disc and says its key; the target is the host's authored copy of it, once that key has a row.
        if (g_key.empty()) {
            if (!ClientSaid(kSlotC1, KeyHead(), &g_key) || g_key.empty()) {
                g_key.clear();
                if (HExpired(kDoneBoundMs)) HostFail(&r, "c1 never said its disc's key");
                return;
            }
            HEnter(HStage::AwaitKey);
        }
        if (void* const found = FindKeyed(g_key)) {
            g_target.Set(found);
            g_eid = EL::Registry::Get().EidForActor(found);
            SayReady(r);
            return;
        }
        if (HExpired(kAnnounceBoundMs)) HostFail(&r, "cell not reached: c1's disc key never got a row");
        return;
    case HStage::AwaitReserved:
        if (coop::prop_hold::ReservedEid(kSlotC1) != static_cast<uint32_t>(g_eid)) {
            if (HExpired(kDoneBoundMs)) HostFail(&r, "c1 never reserved the target");
            return;
        }
        if (r.blocker != Blocker::HostActs) Say(HostLine(r, "c1 reserved"));
        HEnter(HStage::Blocking);
        return;
    case HStage::Blocking:
        if (!Block(r)) {
            if (g_hs == HStage::Blocking && HExpired(kDoneBoundMs)) HostFail(&r, "the blocker was never answered");
            return;
        }
        Say(HostLine(r, "answered"));
        HEnter(HStage::AwaitDone);
        return;
    case HStage::AwaitHeld: {
        void* const actor = g_target.Get();
        const bool held = actor && (r.blocker == Blocker::C2Grab
                                        ? coop::remote_prop::IsActorDrivenByOtherThan(actor, kSlotC1)
                                        : (coop::hook_sync::PlayerHookHolders(actor) & kC2HookBit) != 0);
        if (!held) {
            if (HExpired(kDoneBoundMs)) HostFail(&r, "c2's hold on the target was never read on the host");
            return;
        }
        Say(HostLine(r, r.blocker == Blocker::C2Grab ? "held" : "hooked"));
        HEnter(HStage::AwaitDone);
        return;
    }
    case HStage::AwaitDone: {
        bool failed = false;
        const bool c1 = DoneOk(r, kSlotC1, &failed);
        const bool c2 = failed || !C2SaysDone(r.blocker) || DoneOk(r, kSlotC2, &failed);
        if (failed) return;
        if (!c1 || !c2) {
            if (HExpired(kDoneBoundMs)) HostFail(&r, "a client never said it was done");
            return;
        }
        ClearGrab();
        HEnter(HStage::Judge);
        return;
    }
    case HStage::Judge: {
        std::string why;
        switch (Judge(r, g_eid, &why)) {
        case Judged::Wait:
            if (HExpired(kJudgeBoundMs)) HostFail(&r, "still waiting: " + why);
            return;
        case Judged::Fail:
            HostFail(&r, why);
            return;
        case Judged::Pass:
            UE_LOGI("%s host race step %s PASS -- %s", kTag, r.act.name, why.c_str());
            ResetStep();
            if (++g_hrow < kRaceRowCount) {
                HEnter(kRaceRows[g_hrow].act.target == Target::Client ? HStage::AwaitKey : HStage::Spawn);
                return;
            }
            UE_LOGI("%s host race PASS -- all %u steps held", kTag, static_cast<unsigned>(kRaceRowCount));
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

void RaceHostTick(coop::net::Session* s) {
    if (g_hs == HStage::Done) return;
    KeepGrab();
    const uint64_t now = NowMs();
    if (now < g_hNextMs) return;
    g_hNextMs = now + kPollEveryMs;
    if (g_hs != HStage::Arm && (!s->HasPeerConn(kSlotC1) || !s->HasPeerConn(kSlotC2))) {
        HostFail(nullptr, "a client left before the verdict");
        return;
    }
    HostStep(s);
}

void RaceReset() {
    g_hs = HStage::Arm;
    g_hsMs = g_hNextMs = 0;
    g_hrow = 0;
    ResetStep();
    g_base = RaceBase{};
    RaceClientReset();
}

}  // namespace coop::dev::prop_act_drill::detail
