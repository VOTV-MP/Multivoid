// coop/dev/prop_act_drill_client.cpp -- the act drill's client half: the walk, act and wait step machine. See
// coop/dev/prop_act_drill.h; the core (prop_act_drill.cpp) routes the phase and runs the host half.

#include "coop/dev/prop_act_drill.h"
#include "prop_act_drill_client_steps.h"

#include "coop/comms/chat_feed.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/clear_hand.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat_pace.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_request.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/effects.h"
#include "ue_wrap/actors/food.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace coop::dev::prop_act_drill {
using namespace detail;
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace EL  = coop::element;
namespace PAR = coop::prop_act_request;
namespace PU  = ue_wrap::prop_use;
using ue_wrap::FVector;

constexpr uint8_t kEatAction = 4;   // actionOptionIndex's eat, as the watch reads it
constexpr const char* kSecondTag = " eid2=";   // after the first eid in a pair's ready line
constexpr int kFillMax = 400;            // CLIENT: own spawns pocketed before the inventory is called never full

}  // namespace

// ---- client (taker) ----------------------------------------------------------------------------------------------

std::string g_readyRest;                   // the text after the host's ready line's eid: a snack arm's uses and open
CStep g_cstep = CStep::Arm;
uint64_t g_cStepMs = 0, g_cNextMs = 0, g_nextSayMs = 0;
size_t g_crow = 0;
EL::ElementId g_ceid = EL::kInvalidId, g_ceid2 = EL::kInvalidId;
ue_wrap::CachedObjRef g_mirror, g_mirror2;   // the step's target (a client-made one too), and a pair's second
int g_invBefore = 0;
int g_fillCount = 0;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
std::unique_ptr<coop::director::ClearHand> g_clear;  // a grab row's hand clearing, until the hand reads empty
std::unique_ptr<coop::director::AimedGrab> g_grab;   // a grab row's grab, until the host says it saw the drag
bool g_grabbed = false;
// The state a refused act must leave as it found it, taken at the call: this client's WrongFamily refusals so far
// and the hand's item.
uint32_t g_refusedBefore = 0;
bool g_handHeldBefore = false;
ue_wrap::CachedObjRef g_handBefore;
std::string g_sayLine;
bool g_sayFailed = false;
// An opened row: the host-born food mirrors this client held before the act. An MRE's contents are the host's prop_food_C
// births, so they reach this client as mirrors of its eids; the step passes when one the set lacks has arrived.
std::unordered_set<EL::ElementId> g_foodsBefore;

size_t g_freshContents = 0;   // the opened row's contents that arrived
int g_poisonBefore = 0;       // foodPoison entries live before the poisoned content's bite

void CEnter(CStep s) {
    g_cstep = s;
    g_cStepMs = NowMs();
}

bool CExpired(uint64_t bound) { return NowMs() - g_cStepMs > bound; }

// A failed step is logged now and said to the host at the next chat slot; the phase stops there.
void ClientFail(const Row& row, const std::string& why) {
    UE_LOGW("%s c%d %s FAIL -- %s: %s", kTag, kClientSlot, PhaseName().c_str(), row.name, why.c_str());
    RigSay(End::Fail, std::string(row.name) + ": " + why);
    g_sayLine = DoneHead(row) + "fail " + why.substr(0, 120);
    g_sayFailed = true;
    CEnter(CStep::Say);
}

// A bound ran out with no event to decide on: INVALID, said to the host as such, and the phase stops as it does on a FAIL.
void ClientInvalid(const Row& row, const std::string& why) {
    UE_LOGW("%s c%d %s INVALID -- %s: %s", kTag, kClientSlot, PhaseName().c_str(), row.name, why.c_str());
    RigSay(End::Invalid, std::string(row.name) + ": " + why);
    g_sayLine = DoneHead(row) + "invalid " + why.substr(0, 120);
    g_sayFailed = true;
    CEnter(CStep::Say);
}

void ClientPass(const Row& row, const char* what) {
    UE_LOGI("%s c%d %s step %s PASS -- %s", kTag, kClientSlot, PhaseName().c_str(), row.name, what);
    g_sayLine = DoneHead(row) + "ok";
    g_sayFailed = false;
    CEnter(CStep::Say);
}

namespace {

// The player's hand item name, as the game passes it to a use (holding_name).
bool ReadHoldName(void* player, R::FName* out) {
    static R::InstanceOffset sOff{L"holding_name"};
    const int32_t o = sOff.Of(player);
    if (o < 0) return false;
    std::memcpy(out, static_cast<const uint8_t*>(player) + o, sizeof(*out));
    return true;
}

float g_replayFood = 0.f;   // the eater's food the row wants at the replay: 100 for a full eater, else 0
void FillBeforeReplay(void*) { ue_wrap::vitals::Write(ue_wrap::vitals::Field::Food, g_replayFood); }

void ClientRead(const Row& row, void* player);

// The step's act, once the walk has arrived: setup, the call, and the proof the call was cancelled and asked.
void ClientAct(const Row& row, void* player) {
    void* const mirror = g_mirror.Get();
    if (!mirror) {
        ClientFail(row, "the mirror was gone before the act");
        return;
    }
    if (row.setup) {
        if (const char* why = row.setup(player, mirror)) {
            ClientFail(row, why);
            return;
        }
    }
    // A snack arm with a full eater is fed to the top, so the game's addFood reports full and the use spends nothing.
    const bool fullEater = row.snack && row.snack->full;
    if (row.verb == Verb::Use && !ue_wrap::vitals::Write(ue_wrap::vitals::Field::Food, fullEater ? 100.f : 0.f)) {
        // An eat goes through mainPlayer's addFood, which eats nothing once the save slot's food is at 100 ("Im not
        // hungry"; at health 80 or less a food that heals is still eaten): the rows before have fed this player full.
        ClientFail(row, "the player's hunger could not be emptied before the eat");
        return;
    }
    // The eater's food decays every tick, so a value written here is lower by the time the granted replay runs: the same
    // value is written again right before the replay's verb, the moment the game's addFood reads it.
    g_replayFood = fullEater ? 100.f : 0.f;
    PAR::dev::SetBeforeVerb(row.verb == Verb::Use ? &FillBeforeReplay : nullptr);
    ClearHint();
    g_refusedBefore = PAR::dev::RefusedCount(coop::net::PropActReason::WrongFamily);
    E::MainPlayerGrabState handBefore{};
    g_handHeldBefore = E::ReadMainPlayerGrabState(player, handBefore) && handBefore.holdingActor;
    g_handBefore.Set(g_handHeldBefore ? handBefore.holdingActor : nullptr);
    if (row.target == Target::HostTwo) {
        // The second prop dies inside the replay's latch, so it goes through the destroy seam as any destroy does.
        if (!g_mirror2.Get()) {
            ClientFail(row, "the second mirror was gone before the act");
            return;
        }
        PAR::dev::DestroyInsideNextReplay(g_mirror2.Get());
    }
    if (row.ends == Ends::Opened) {
        g_foodsBefore.clear();
        FoodMirrorsNotIn({}, &g_foodsBefore);
    }
    if (const char* why = RunVerb(row, player, mirror)) {
        ClientFail(row, why);
        return;
    }
    if (!row.asks) {
        // The client's own prop or a native subclass: the call runs at once and opens no request.
        if (PAR::dev::PendingId() != 0) {
            ClientFail(row, "the call opened a request: it was not left to run natively");
            return;
        }
        UE_LOGI("%s c%d %s %s: the call ran natively, no request is open", kTag, kClientSlot, PhaseName().c_str(), row.name);
        ClientRead(row, player);
        return;
    }
    // The use's own call has returned. A snack's use that ran unasked (the family routed native) is seen here, at the call:
    // no request is open and the copy changed or ended, which is the event the red arm ends on.
    if (!g_mirror.Alive()) {
        ClientFail(row, row.snack ? "the use ran unasked on this client's copy: it ended with no request sent"
                                  : "the mirror was gone right after the call: the act was not cancelled at the call");
        return;
    }
    if (PAR::dev::PendingId() == 0) {
        ClientFail(row, row.snack ? "the use ran unasked on this client's copy: no request was sent and its state is " +
                                        SnackCopyNow(g_mirror.Get())
                                  : std::string("the call opened no request"));
        return;
    }
    UE_LOGI("%s c%d %s %s: the call was cancelled and asked (request %u)", kTag, kClientSlot, PhaseName().c_str(), row.name,
            static_cast<unsigned>(PAR::dev::PendingId()));
    CEnter(CStep::AwaitReply);
}

// The take bailed on the full inventory: the prop is where it was, the inventory holds what it held, and the game said so.
void ClientReadStays(const Row& row) {
    if (!g_mirror.Alive() || !EL::LivePropActor(g_ceid)) {
        ClientFail(row, "the mirror is gone after a take that bailed");
        return;
    }
    const int now = InventoryCount(row.leaf);
    if (now != g_invBefore) {
        ClientFail(row, "the inventory changed on a take that bailed: " + std::to_string(g_invBefore) + " before, " +
                            std::to_string(now) + " after");
        return;
    }
    if (!HintSeen()) {
        ClientFail(row, "the game showed no 'Not enough space' hint");
        return;
    }
    ClientPass(row, "asked, granted, the pocket bailed with the full-inventory hint; the mirror and the inventory stayed");
}

// The host refused the act (WrongFamily): the refusal came once, no request is open, and the mirror, the inventory and
// the hand are as they were before the call.
void ClientReadRefused(const Row& row, void* player) {
    const uint32_t refused = PAR::dev::RefusedCount(coop::net::PropActReason::WrongFamily) - g_refusedBefore;
    if (refused != 1) {
        ClientFail(row, std::to_string(refused) + " WrongFamily refusals reached this client, 1 expected");
        return;
    }
    if (PAR::dev::PendingId() != 0) {
        ClientFail(row, "a request is still open after the refusal");
        return;
    }
    if (!g_mirror.Alive() || !EL::LivePropActor(g_ceid)) {
        ClientFail(row, "the mirror is gone after a refused act");
        return;
    }
    const int now = InventoryCount(row.leaf);
    if (now != g_invBefore) {
        ClientFail(row, "the inventory changed on a refused act: " + std::to_string(g_invBefore) + " before, " +
                            std::to_string(now) + " after");
        return;
    }
    E::MainPlayerGrabState hand{};
    const bool held = E::ReadMainPlayerGrabState(player, hand) && hand.holdingActor;
    if (held != g_handHeldBefore || (held && !g_handBefore.Is(hand.holdingActor))) {
        ClientFail(row, "the hand changed on a refused act");
        return;
    }
    ClientPass(row, "asked, refused WrongFamily; the mirror, the inventory and the hand stayed");
}

// The call is over: no request is pending (the host's answer is in and the replay has run, or the call was native).
// What this client's copies show now.
void ClientRead(const Row& row, void* player) {
    if (row.ends == Ends::Unchecked) {
        ClientPass(row, "the call ran natively and opened no request");
        return;
    }
    if (row.ends == Ends::NativeGone) {
        // The eat ran: a request would have been caught at the call, so the copy is gone only if the game ate it.
        if (g_mirror.Alive() || (g_ceid != EL::kInvalidId && EL::LivePropActor(g_ceid))) {
            ClientFail(row, "the native eat did not consume this client's copy");
            return;
        }
        ClientPass(row, "the call ran natively, opened no request and ate the copy");
        return;
    }
    if (row.ends == Ends::Applied || row.ends == Ends::Ended || row.ends == Ends::Respawn) {
        CEnter(CStep::AwaitCopy);   // the host's record and birth reach this client after the call: waited for, bounded
        return;
    }
    if (row.ends == Ends::Stays) {
        ClientReadStays(row);
        return;
    }
    if (row.ends == Ends::Refused) {
        ClientReadRefused(row, player);
        return;
    }
    if (g_mirror.Alive()) {
        ClientFail(row, std::string("the mirror stayed after the host's answer") + MirrorState(g_mirror.Get()));
        return;
    }
    if (g_ceid != EL::kInvalidId && EL::LivePropActor(g_ceid)) {
        ClientFail(row, "the eid still names a prop on this client");
        return;
    }
    if (g_ceid2 != EL::kInvalidId && (g_mirror2.Alive() || EL::LivePropActor(g_ceid2))) {
        ClientFail(row, "the second prop stayed after the replay destroyed it");
        return;
    }
    if (row.ends == Ends::Inventory) {
        const int now = InventoryCount(row.leaf);
        if (now != g_invBefore + 1) {
            ClientFail(row, "the inventory did not gain the item: " + std::to_string(g_invBefore) + " before, " +
                                std::to_string(now) + " after");
            return;
        }
    } else if (row.ends == Ends::Hand) {
        E::MainPlayerGrabState hand{};
        if (!E::ReadMainPlayerGrabState(player, hand) || !hand.holdingActor || R::ClassNameOf(hand.holdingActor) != row.leaf) {
            ClientFail(row, "the hand does not hold the item");
            return;
        }
        if (row.snack) {
            if (const char* why = SnackHeldVerdict(hand.holdingActor, g_readyRest)) {
                ClientFail(row, why);
                return;
            }
        }
    }
    if (row.ends == Ends::Opened) {
        CEnter(CStep::AwaitContents);
        return;
    }
    ClientPass(row, row.asks ? "asked, granted, replayed; the mirror is gone and the item is where the verb puts it"
                             : "ran natively with no request; the prop is gone and the item is where the verb puts it");
}

// Walks to the mirror with the director, the bound on the walk being the director's own.
void BeginWalk(const Row& row, void* mirror) {
    FVector at{};
    if (!E::TryGetActorLocation(mirror, at)) {
        ClientFail(row, "the mirror's place could not be read");
        return;
    }
    g_walk = coop::director::StartBackgroundWalk(at, kReachCm, kWalkDeadlineS);
    UE_LOGI("%s c%d %s %s: walking to eid %u at (%.0f, %.0f, %.0f)", kTag, kClientSlot, PhaseName().c_str(), row.name,
            static_cast<unsigned>(g_ceid), at.X, at.Y, at.Z);
    CEnter(CStep::Walk);
}

// A client-made target: the row spawns it and reads its cell back before the call.
void ClientOwn(const Row& row, void* player) {
    void* own = nullptr;
    if (const char* why = row.make(player, &own)) {
        ClientFail(row, why);
        return;
    }
    g_mirror.Set(own);
    g_invBefore = InventoryCount(row.leaf);
    if (g_invBefore < 0) {
        ClientFail(row, "the inventory could not be read");
        return;
    }
    ClientAct(row, player);
}

// The inventory is filled, one own spawn pocketed per tick, until the native return is false: the state the full row's
// take bails on.
void ClientFill(const Row& row, void* player) {
    void* const mirror = g_mirror.Get();
    if (!mirror) {
        ClientFail(row, "the mirror was gone while the inventory filled");
        return;
    }
    if (g_fillCount >= kFillMax) {
        ClientFail(row, "the inventory still took an item after " + std::to_string(g_fillCount) + " own spawns");
        return;
    }
    bool full = false;
    if (const char* why = row.fillStep(player, &full)) {
        ClientFail(row, why);
        return;
    }
    if (PAR::dev::PendingId() != 0) {
        ClientFail(row, "pocketing an own spawn opened a request");
        return;
    }
    ++g_fillCount;
    if (!full) return;
    g_invBefore = InventoryCount(row.leaf);
    UE_LOGI("%s c%d %s %s: the inventory is full after %d own spawns", kTag, kClientSlot, PhaseName().c_str(), row.name,
            g_fillCount);
    BeginWalk(row, mirror);
}

// The game's grab chain runs on an empty hand: the director's ClearHand takes the item out of it first (the drop key,
// then its effect-seam fallback when that is measured inert), ticked every game tick.
void BeginClear() {
    g_clear = std::make_unique<coop::director::ClearHand>();
    CEnter(CStep::ClearHand);
}

void BeginGrab(void* player) {
    g_grab = std::make_unique<coop::director::AimedGrab>(player, g_mirror.Get());
    g_grabbed = false;
    CEnter(CStep::Grab);
}

}  // namespace

namespace detail {

// The text after `head` in the first row of this client's feed that carries it, a retained row counted only when
// `liveOnly` is false.
static bool FeedSaid(const std::string& head, std::string* rest, bool liveOnly) {
    bool found = false;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        const size_t at = found || (liveOnly && r.retained) ? std::string_view::npos : r.line.find(head);
        if (at == std::string_view::npos) return;
        found = true;
        if (rest) *rest = std::string(r.line.substr(at + head.size()));
    });
    return found;
}

bool HostSaid(const std::string& head, std::string* rest) { return FeedSaid(head, rest, false); }

bool HostSaidLive(const std::string& head, std::string* rest) { return FeedSaid(head, rest, true); }

bool OwnJoinOver() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

int InventoryCount(const wchar_t* leaf) {
    ue_wrap::inventory::LivePersonalStore store;
    if (!ue_wrap::inventory::ReadLivePersonalStore(store)) return -1;
    int n = 0;
    for (const auto& rec : store.records)
        if (rec.className == leaf) ++n;
    return n;
}

void* RootOf(void* actor) {
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(actor), L"K2_GetRootComponent");
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !ue_wrap::Call(actor, f)) return nullptr;
    return f.Get<void*>(L"ReturnValue");
}

// The row's verb on this player and the mirror, the call a press makes. Null on success, else why the call could not run.
const char* RunVerb(const Row& row, void* player, void* mirror) {
    bool flag = false;
    if (row.verb == Verb::Pocket)
        return E::CallPutObjectInventory2(player, mirror, false, flag) ? nullptr : "putObjectInventory2 could not run";
    if (row.verb == Verb::Hold)
        return E::CallMainPlayerHoldObject(player, mirror, flag) ? nullptr : "Hold Object could not run";
    void* const root = RootOf(mirror);
    FVector at{};
    if (!root || !E::TryGetActorLocation(mirror, at)) return "the mirror's root or place could not be read";
    if (row.verb == Verb::Use)
        return PU::CallActionOptionIndex(mirror, player, root, at, kEatAction, root) ? nullptr
                                                                                  : "actionOptionIndex could not run";
    E::MainPlayerGrabState hand{};
    R::FName name{};
    if (!E::ReadMainPlayerGrabState(player, hand) || !ReadHoldName(player, &name)) return "the hand could not be read";
    return PU::CallPlayerUsedOn(mirror, player, root, at, root, hand.holdingActor, name) ? nullptr
                                                                                       : "playerUsedOn could not run";
}

void ClientTick(coop::net::Session* s, const Phase& ph) {
    if (g_cstep == CStep::Done) return;
    // The host ended the phase on its own verdict: nothing more is run here, and no bound of this client says a second one.
    if (HostEnded()) {
        g_cstep = CStep::Done;
        return;
    }
    void* const player = coop::players::Registry::Get().Local();
    if (!player) return;
    const uint64_t now = NowMs();
    const Row& row = ph.rows[g_crow < ph.count ? g_crow : 0];
    switch (g_cstep) {
    case CStep::Arm: {
        const uint8_t self = coop::players::Registry::Get().LocalPeerId();
        if (!OwnJoinOver() || !s->connected() || self == 0) return;
        if (self != kClientSlot) {
            UE_LOGI("%s c%u %s idle -- this phase runs on c%d only", kTag, static_cast<unsigned>(self), ph.name, kClientSlot);
            g_cstep = CStep::Done;
            return;
        }
        CEnter(CStep::AwaitReady);
        return;
    }
    case CStep::AwaitReady: {
        if (now < g_cNextMs) return;
        g_cNextMs = now + kPollEveryMs;
        std::string rest;
        if (!HostSaid(ReadyHead(row), &rest)) {
            if (CExpired(kReadyBoundMs)) ClientInvalid(row, "the host never said the step was ready");
            return;
        }
        g_ceid = g_ceid2 = EL::kInvalidId;
        if (row.target == Target::Client) {
            ClientOwn(row, player);
            return;
        }
        g_ceid = static_cast<EL::ElementId>(std::strtoul(rest.c_str(), nullptr, 10));
        g_readyRest = rest;
        if (row.target == Target::HostTwo) {
            const size_t at = rest.find(kSecondTag);
            if (at == std::string::npos) {
                ClientFail(row, "the host's line named no second prop");
                return;
            }
            g_ceid2 = static_cast<EL::ElementId>(std::strtoul(rest.c_str() + at + std::strlen(kSecondTag), nullptr, 10));
        }
        CEnter(CStep::Resolve);
        return;
    }
    case CStep::Resolve: {
        void* const mirror = EL::LivePropActor(g_ceid);
        void* const second = g_ceid2 != EL::kInvalidId ? EL::LivePropActor(g_ceid2) : nullptr;
        if (!mirror || (g_ceid2 != EL::kInvalidId && !second)) {
            if (CExpired(kMirrorBoundMs)) ClientInvalid(row, "the host's prop never reached this client");
            return;
        }
        if ((row.fillStep || row.ends == Ends::Stays) && !HintWatchLive()) {
            if (CExpired(kMirrorBoundMs)) ClientInvalid(row, "the addHint watch never went live");
            return;
        }
        g_invBefore = InventoryCount(row.leaf);
        if (g_invBefore < 0) {
            ClientFail(row, "the inventory could not be read");
            return;
        }
        g_mirror.Set(mirror);
        g_mirror2.Set(second);
        if (row.fillStep) {
            g_fillCount = 0;
            CEnter(CStep::Fill);
            return;
        }
        BeginWalk(row, mirror);
        return;
    }
    case CStep::Fill:
        ClientFill(row, player);
        return;
    case CStep::Walk: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) return;
        g_walk.reset();
        if (st == 2) {
            ClientInvalid(row, "the walk to the mirror did not arrive");
            return;
        }
        CEnter(CStep::AwaitGather);
        return;
    }
    case CStep::AwaitGather: {
        // A host mirror still driven is parked with physics off, and a take of it bails: act when a press could gather it.
        if (now < g_cNextMs) return;
        g_cNextMs = now + kPollEveryMs;
        if (g_mirror.Get() && !MirrorGatherable(g_mirror.Get(), row.verb)) {
            if (CExpired(kGatherBoundMs))
                ClientInvalid(row, "the mirror never became gatherable here (the host's drive did not end)");
            return;
        }
        // A snack arm acts on a mirror that already holds the host's uses and open (its record is published after the
        // host's write): the instrument waits for it, bounded, and the arm is INVALID when it never does.
        if (row.snack && !SnackMirrorHoldsReady(g_mirror.Get(), g_readyRest)) {
            if (CExpired(kCopyBoundMs)) ClientInvalid(row, "the mirror never held the host's uses and open from the ready line");
            return;
        }
        if (row.grab) {
            BeginClear();
            return;
        }
        ClientAct(row, player);
        return;
    }
    case CStep::ClearHand: {
        // The clearing is ticked on every tick, like the grab after it.
        if (!g_mirror.Get()) {
            ClientFail(row, "the mirror was gone before the grab");
            return;
        }
        const coop::director::ClearState st = g_clear->Tick();
        if (st == coop::director::ClearState::Failed) {
            ClientFail(row, "the hand did not clear before the grab");
            return;
        }
        if (st == coop::director::ClearState::Working) {
            if (CExpired(kHandClearBoundMs)) ClientInvalid(row, "the hand did not clear before the grab");
            return;
        }
        g_clear.reset();
        BeginGrab(player);
        return;
    }
    case CStep::Grab: {
        // The game's grab, ticked on every tick; once it has taken, the grabbed line goes through the drills' chat
        // pacing and the host says when it sees the prop driven.
        if (!g_mirror.Get()) {
            ClientFail(row, "the mirror was gone before the grab");
            return;
        }
        if (!g_grabbed) {
            const coop::director::GrabState st = g_grab->Tick();
            if (st == coop::director::GrabState::Failed) {
                // A grab that found no aim never reached the pocket it was set up for: INVALID. The chain that ran and left
                // the prop out of the hand is the grab failing, a FAIL.
                const std::string why = std::string("the grab did not take: ") + g_grab->Why();
                if (g_grab->NoAim()) ClientInvalid(row, why);
                else ClientFail(row, why);
                return;
            }
            if (st == coop::director::GrabState::Working) {
                if (CExpired(kGrabBoundMs)) ClientInvalid(row, "the grab neither took nor failed in time");
                return;
            }
            g_grabbed = true;
            UE_LOGI("%s c%d %s %s: grabbed after %d aim pose(s)", kTag, kClientSlot, PhaseName().c_str(), row.name,
                    g_grab->AimPoses());
        }
        if (now < g_nextSayMs) return;
        Say(DragHead(row, false) + "grabbed");
        g_nextSayMs = now + coop::dev::kTalkEveryMs;
        CEnter(CStep::AwaitDriven);
        return;
    }
    case CStep::AwaitDriven:
        if (now < g_cNextMs) return;
        g_cNextMs = now + kPollEveryMs;
        if (!HostSaid(DragHead(row, true) + "driven", nullptr)) {
            if (CExpired(kDrivenBoundMs)) ClientInvalid(row, "the host never saw the drag");
            return;
        }
        ClientAct(row, player);
        return;
    case CStep::AwaitReply:
        if (PAR::dev::PendingId() != 0) {
            if (CExpired(kReplyBoundMs)) ClientInvalid(row, "the request was still pending");
            return;
        }
        ClientRead(row, player);
        return;
    case CStep::AwaitContents:
        StepAwaitContents(row, player, now);
        return;
    case CStep::AwaitPoison:
        StepAwaitPoison(row, player, now);
        return;
    case CStep::AwaitCopy:
        StepAwaitCopy(row, player, now);
        return;
    case CStep::AwaitUpright:
        StepAwaitUpright(row, player, now);
        return;
    case CStep::Say: {
        // A client's chat passes the host's rate limit only at the drills' spacing; a refused line is lost for good.
        if (now < g_nextSayMs) return;
        // The verdict of the step goes out at once: a decided PASS is never delayed, or turned INVALID, by a ragdoll.
        Say(g_sayLine);
        g_nextSayMs = now + coop::dev::kTalkEveryMs;
        g_mirror.Reset();
        g_mirror2.Reset();
        g_clear.reset();
        g_grab.reset();
        if (g_sayFailed) {
            HintWatchOff();
            g_cstep = CStep::Done;
            return;
        }
        if (g_crow + 1 < ph.count) {
            // Another row follows: the host places its target by this player's puppet, so it waits for the line that says
            // the player stands (StepAwaitUpright). On the last row nothing waits.
            CEnter(CStep::AwaitUpright);
            return;
        }
        UE_LOGI("%s c%d %s PASS -- all %u steps held on this client", kTag, kClientSlot, ph.name,
                static_cast<unsigned>(ph.count));
        HintWatchOff();
        g_cstep = CStep::Done;
        return;
    }
    case CStep::Done:
        return;
    }
}

void ClientReset() {
    g_cstep = CStep::Arm;
    g_cStepMs = g_cNextMs = g_nextSayMs = 0;
    g_crow = 0;
    g_ceid = g_ceid2 = EL::kInvalidId;
    g_mirror.Reset();
    g_mirror2.Reset();
    g_invBefore = 0;
    g_fillCount = 0;
    ClearHint();
    HintWatchOff();
    g_walk.reset();
    g_clear.reset();
    g_grab.reset();
    g_grabbed = false;
    g_refusedBefore = 0;
    g_handHeldBefore = false;
    g_handBefore.Reset();
    g_sayLine.clear();
    g_sayFailed = false;
    g_foodsBefore.clear();
    g_freshContents = 0;
    g_poisonBefore = 0;
    g_readyRest.clear();
}

}  // namespace detail

}  // namespace coop::dev::prop_act_drill
