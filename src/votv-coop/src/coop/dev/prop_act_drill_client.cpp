// coop/dev/prop_act_drill_client.cpp -- the act drill's client half: the walk, act and wait step machine. See
// coop/dev/prop_act_drill.h; the core (prop_act_drill.cpp) routes the phase and runs the host half.

#include "coop/dev/prop_act_drill.h"

#include "coop/comms/chat_feed.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat_pace.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_request.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/prop_use.h"
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

constexpr float kReachCm = 120.f;        // the client's stop beside the mirror
constexpr int kWalkDeadlineS = 60;       // the director's own bound on the walk

// Bounds: each ends a step that did not get its line or its count, and FAILs it.
constexpr uint64_t kReadyBoundMs = 120000;   // CLIENT: a step's start to the host's ready line
constexpr uint64_t kMirrorBoundMs = 60000;   // CLIENT: the ready line to the mirror being here
constexpr uint64_t kReplyBoundMs = 30000;    // CLIENT: the call to no request pending

// The text after `head` in the first row of this client's feed that carries it.
bool HostSaid(const std::string& head, std::string* rest) {
    bool found = false;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        const size_t at = found ? std::string_view::npos : r.line.find(head);
        if (at == std::string_view::npos) return;
        found = true;
        if (rest) *rest = std::string(r.line.substr(at + head.size()));
    });
    return found;
}

// ---- client (taker) ----------------------------------------------------------------------------------------------

enum class CStep : uint8_t { Arm, AwaitReady, Resolve, Fill, Walk, AwaitReply, Say, Done };
CStep g_cstep = CStep::Arm;
uint64_t g_cStepMs = 0, g_cNextMs = 0, g_nextSayMs = 0;
size_t g_crow = 0;
EL::ElementId g_ceid = EL::kInvalidId, g_ceid2 = EL::kInvalidId;
ue_wrap::CachedObjRef g_mirror, g_mirror2;   // the step's target (a client-made one too), and a pair's second
int g_invBefore = 0;
int g_fillCount = 0;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
std::string g_sayLine;
bool g_sayFailed = false;

void CEnter(CStep s) {
    g_cstep = s;
    g_cStepMs = NowMs();
}

bool CExpired(uint64_t bound) { return NowMs() - g_cStepMs > bound; }

// A failed step is logged now and said to the host at the next chat slot; the phase stops there.
void ClientFail(const Row& row, const std::string& why) {
    UE_LOGW("%s c%d %s FAIL -- %s: %s", kTag, kClientSlot, PhaseName().c_str(), row.name, why.c_str());
    g_sayLine = DoneHead(row) + "fail " + why.substr(0, 120);
    g_sayFailed = true;
    CEnter(CStep::Say);
}

void ClientPass(const Row& row, const char* what) {
    UE_LOGI("%s c%d %s step %s PASS -- %s", kTag, kClientSlot, PhaseName().c_str(), row.name, what);
    g_sayLine = DoneHead(row) + "ok";
    g_sayFailed = false;
    CEnter(CStep::Say);
}

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

// The player's hand item name, as the game passes it to a use (holding_name).
bool ReadHoldName(void* player, R::FName* out) {
    static R::InstanceOffset sOff{L"holding_name"};
    const int32_t o = sOff.Of(player);
    if (o < 0) return false;
    std::memcpy(out, static_cast<const uint8_t*>(player) + o, sizeof(*out));
    return true;
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
    ClearHint();
    if (row.target == Target::HostTwo) {
        // The second prop dies inside the replay's latch, so it goes through the destroy seam as any destroy does.
        if (!g_mirror2.Get()) {
            ClientFail(row, "the second mirror was gone before the act");
            return;
        }
        PAR::dev::DestroyInsideNextReplay(g_mirror2.Get());
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
    if (!g_mirror.Alive()) {
        ClientFail(row, "the mirror was gone right after the call: the act was not cancelled at the call");
        return;
    }
    if (PAR::dev::PendingId() == 0) {
        ClientFail(row, "the call opened no request");
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

// The call is over: no request is pending (the host's answer is in and the replay has run, or the call was native).
// What this client's copies show now.
void ClientRead(const Row& row, void* player) {
    if (row.ends == Ends::Unchecked) {
        ClientPass(row, "the call ran natively and opened no request");
        return;
    }
    if (row.ends == Ends::Stays) {
        ClientReadStays(row);
        return;
    }
    if (g_mirror.Alive()) {
        ClientFail(row, "the mirror stayed after the host's answer");
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


}  // namespace

namespace detail {

void ClientTick(coop::net::Session* s, const Phase& ph) {
    if (g_cstep == CStep::Done) return;
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
            if (CExpired(kReadyBoundMs)) ClientFail(row, "the host never said the step was ready");
            return;
        }
        g_ceid = g_ceid2 = EL::kInvalidId;
        if (row.target == Target::Client) {
            ClientOwn(row, player);
            return;
        }
        g_ceid = static_cast<EL::ElementId>(std::strtoul(rest.c_str(), nullptr, 10));
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
            if (CExpired(kMirrorBoundMs)) ClientFail(row, "the host's prop never reached this client");
            return;
        }
        if (row.fillStep && !HintWatchLive()) {
            if (CExpired(kMirrorBoundMs)) ClientFail(row, "the addHint watch never went live");
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
            ClientFail(row, "the walk to the mirror did not arrive");
            return;
        }
        ClientAct(row, player);
        return;
    }
    case CStep::AwaitReply:
        if (PAR::dev::PendingId() != 0) {
            if (CExpired(kReplyBoundMs)) ClientFail(row, "the request was still pending");
            return;
        }
        ClientRead(row, player);
        return;
    case CStep::Say: {
        // A client's chat passes the host's rate limit only at the drills' spacing; a refused line is lost for good.
        if (now < g_nextSayMs) return;
        Say(g_sayLine);
        g_nextSayMs = now + coop::dev::kTalkEveryMs;
        g_mirror.Reset();
        g_mirror2.Reset();
        if (g_sayFailed) {
            g_cstep = CStep::Done;
            return;
        }
        if (++g_crow < ph.count) {
            CEnter(CStep::AwaitReady);
            return;
        }
        UE_LOGI("%s c%d %s PASS -- all %u steps held on this client", kTag, kClientSlot, ph.name,
                static_cast<unsigned>(ph.count));
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
    g_walk.reset();
    g_sayLine.clear();
    g_sayFailed = false;
}

}  // namespace detail

}  // namespace coop::dev::prop_act_drill
