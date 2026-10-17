// coop/dev/prop_act_drill.cpp -- see coop/dev/prop_act_drill.h.

#include "coop/dev/prop_act_drill.h"

#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_log.h"
#include "coop/comms/chat_sync.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/kerfur_entity.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat_pace.h"
#include "coop/dev/spawn_npc.h"
#include "coop/dispatch/event_feed.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_host.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
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
namespace PAH = coop::prop_act_host;
namespace PAR = coop::prop_act_request;
namespace PU  = ue_wrap::prop_use;
namespace KE  = coop::kerfur_entity;
using ue_wrap::FVector;

constexpr const char* kTag = "[ACT-DRILL]";
constexpr uint8_t kEatAction = 4;   // actionOptionIndex's eat, as the watch reads it
constexpr const wchar_t* kKerfurNpc = L"kerfurOmega_C";
constexpr const char* kSecondTag = " eid2=";   // after the first eid in a pair's ready line

// The host's spawn: along its player's facing, the first distance whose column has a floor; the floor found by a trace.
constexpr float kAheadCm[] = {200.f, 150.f, 250.f};
constexpr float kDropCm = 40.f;          // above the floor, so the prop settles
constexpr float kFloorDownCm = 400.f;    // how far under the player's middle the floor is looked for
constexpr float kMinColumnCm = 20.f;     // a column whose floor is nearer than this to its start began inside geometry
constexpr float kPairGapCm = 60.f;       // the second prop of a pair, further along the same line
constexpr float kNpcLiftCm = 60.f;       // a kerfur NPC is placed above the floor so its capsule clears it
constexpr int kFillMax = 400;            // CLIENT: own spawns pocketed before the inventory is called never full

constexpr float kReachCm = 120.f;        // the client's stop beside the mirror
constexpr int kWalkDeadlineS = 60;       // the director's own bound on the walk

// Bounds: each ends a step that did not get its line or its count, and FAILs it.
constexpr uint64_t kPollEveryMs = 200;       // a chat walk, a few times a second
constexpr uint64_t kAnnounceBoundMs = 60000; // HOST: the spawn to the target having a row
constexpr uint64_t kDoneBoundMs = 180000;    // HOST: the ready line to the client's done line
constexpr uint64_t kJudgeBoundMs = 30000;    // HOST: the done line to the counters it must show
constexpr uint64_t kReadyBoundMs = 120000;   // CLIENT: a step's start to the host's ready line
constexpr uint64_t kMirrorBoundMs = 60000;   // CLIENT: the ready line to the mirror being here
constexpr uint64_t kReplyBoundMs = 30000;    // CLIENT: the call to no request pending

uint64_t NowMs() { return ::GetTickCount64(); }

const std::string& PhaseName() {
    static const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::prop_act_drill);
    return v;
}

const Phase* ActivePhase() {
    static const Phase* const p = PhaseName() == "honest" ? &Honest() : nullptr;
    return p;
}

// The lines the two peers say to each other in the lobby chat.
std::string ReadyHead(const Row& r) { return std::string(kTag) + " host " + r.name + " ready eid="; }
std::string DoneHead(const Row& r) { return std::string(kTag) + " c" + std::to_string(kClientSlot) + " " + r.name + " done "; }

void Say(const std::string& line) { coop::chat_sync::QueueSend(line); }

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

// The text after `head` in the first row of the lobby record that the client's slot said.
bool ClientSaid(const std::string& head, std::string* rest) {
    bool found = false;
    coop::chat_log::ForEach([&](const coop::chat_log::Row& r) {
        if (found || r.slot != kClientSlot || r.text.compare(0, head.size(), head) != 0) return;
        found = true;
        if (rest) *rest = r.text.substr(head.size());
    });
    return found;
}

// ---- host (judge) ------------------------------------------------------------------------------------------------

enum class HStep : uint8_t { Arm, Spawn, Announce, AwaitDone, Judge, Done };
HStep g_hstep = HStep::Arm;
uint64_t g_hStepMs = 0, g_hNextMs = 0;
size_t g_hrow = 0;
ue_wrap::CachedObjRef g_target, g_target2;
EL::ElementId g_eid = EL::kInvalidId, g_eid2 = EL::kInvalidId;
EL::ElementId g_kerfurId = EL::kInvalidId;   // HostKerfur: the NPC's record, which the prop keeps through the turn-off
bool g_turnedOff = false;
Baseline g_base;

void HEnter(HStep s) {
    g_hstep = s;
    g_hStepMs = NowMs();
}

bool HExpired(uint64_t bound) { return NowMs() - g_hStepMs > bound; }

void HostFail(const Row* row, const std::string& why) {
    UE_LOGW("%s host %s FAIL -- %s%s%s", kTag, PhaseName().c_str(), row ? row->name : "", row ? ": " : "", why.c_str());
    g_hstep = HStep::Done;
}

// The floor under the column at (x, y): the first static or dynamic thing a trace meets going down, every player body
// ignored. False when none is met or the column began inside geometry.
bool FloorAt(void* ctx, float x, float y, float fromZ, float* floorZ) {
    const uint8_t types[2] = {0, 1};
    void* ignore[coop::players::kMaxPeers];
    const int nIgnore = coop::players::Registry::Get().LiveBodies(ignore, coop::players::kMaxPeers);
    ue_wrap::trace::Hit hit;
    if (!ue_wrap::trace::LineHitForObjects(ctx, {x, y, fromZ}, {x, y, fromZ - kFloorDownCm}, types, 2, ignore, nIgnore,
                                           &hit) ||
        !hit.blocked || fromZ - hit.point.Z < kMinColumnCm)
        return false;
    *floorZ = hit.point.Z;
    return true;
}

// Spawns the row's class on the floor in front of the host's player, `extraCm` further along when it is a pair's
// second. A kerfur row spawns the NPC. Null on success, else why not.
const char* SpawnTarget(const Row& row, float extraCm, void** out) {
    void* const player = coop::players::Registry::Get().Local();
    FVector at{};
    if (!player || !E::TryGetActorLocation(player, at)) return "the host's player place could not be read";
    FVector fwd = E::GetActorForwardVector(player);
    const float len = std::sqrt(fwd.X * fwd.X + fwd.Y * fwd.Y);
    if (len < 0.1f) return "the host's player facing could not be read";
    const bool npc = row.target == Target::HostKerfur;
    void* const cls = npc ? nullptr : ue_wrap::asset_load::LoadObjectByPath(row.path);
    if (!npc && !cls) return "the target's class did not load";
    for (const float base : kAheadCm) {
        const float ahead = base + extraCm;
        const float x = at.X + fwd.X / len * ahead, y = at.Y + fwd.Y / len * ahead;
        float floorZ = 0.f;
        if (!FloorAt(player, x, y, at.Z, &floorZ)) continue;
        void* const p = npc ? coop::dev::spawn_npc::SpawnAt(kKerfurNpc, {x, y, floorZ + kDropCm + kNpcLiftCm})
                            : E::SpawnActor(cls, {x, y, floorZ + kDropCm});
        if (!p) return "the target did not spawn";
        UE_LOGI("%s host %s spawned %ls at (%.0f, %.0f, %.0f), %.0f uu ahead of its player", kTag, row.name,
                npc ? kKerfurNpc : row.leaf, x, y, floorZ + kDropCm, ahead);
        *out = p;
        return nullptr;
    }
    return "no floor was found in front of the host's player";
}

// The counters of the client's slot as the step begins.
Baseline TakeBaseline(EL::ElementId eid) {
    Baseline b;
    b.destroys = PAH::CommitsOf(kClientSlot, PAH::internal::CommitStep::Destroy);
    b.aborts = PAH::CommitsOf(kClientSlot, PAH::internal::CommitStep::Abort);
    b.rawDestroys = coop::event_feed::DrainedFrom(kClientSlot, coop::net::ReliableKind::PropDestroy);
    b.answered = PAH::AnsweredTotal(kClientSlot);
    b.kerfurBound = eid != EL::kInvalidId && KE::GetKerfurIdForEid(eid) != EL::kInvalidId;
    return b;
}

unsigned Wire(EL::ElementId eid) { return eid == EL::kInvalidId ? 0u : static_cast<unsigned>(eid); }

// The step's targets have rows: g_eid and g_eid2 set. False while one has not, or after a FAIL.
bool TargetAnnounced(const Row& row, ue_wrap::CachedObjRef& ref, EL::ElementId* eid) {
    void* const actor = ref.Get();
    if (!actor) {
        HostFail(&row, "the spawned prop was gone before it was announced");
        return false;
    }
    *eid = EL::Registry::Get().EidForActor(actor);
    if (*eid != EL::kInvalidId) return true;
    if (HExpired(kAnnounceBoundMs)) HostFail(&row, "the spawned prop was never announced");
    return false;
}

// HostKerfur: the NPC is announced and holds a kerfur record, it is turned off, and its prop is found (g_target is the
// NPC until then and the prop after). False while waiting, or after a FAIL.
bool KerfurPropReady(const Row& row) {
    if (!g_turnedOff) {
        void* const npc = g_target.Get();
        if (!npc) {
            HostFail(&row, "the kerfur NPC was gone before it was turned off");
            return false;
        }
        const EL::ElementId npcEid = EL::Registry::Get().EidForActor(npc);
        g_kerfurId = npcEid != EL::kInvalidId ? KE::GetKerfurIdForEid(npcEid) : EL::kInvalidId;
        if (g_kerfurId == EL::kInvalidId) {
            if (HExpired(kAnnounceBoundMs)) HostFail(&row, "the kerfur NPC never got a kerfur record");
            return false;
        }
        if (!TurnOffKerfur(npc)) {
            HostFail(&row, "dropKerfurProp could not be called on the kerfur NPC");
            return false;
        }
        g_turnedOff = true;
        UE_LOGI("%s host %s: kerfur record %u turned off", kTag, row.name, static_cast<unsigned>(g_kerfurId));
        return false;
    }
    void* const prop = FindKerfurProp(g_kerfurId);
    if (!prop) {
        if (HExpired(kAnnounceBoundMs)) HostFail(&row, "the turned-off kerfur's prop was never found");
        return false;
    }
    g_target.Set(prop);
    return true;
}

// The targets are announced: take the counters, say the step is ready (a client-made target has no eid, said 0).
void SayReady(const Row& row) {
    g_base = TakeBaseline(g_eid);
    std::string line = ReadyHead(row) + std::to_string(Wire(g_eid));
    if (g_eid2 != EL::kInvalidId) line += " eid2=" + std::to_string(Wire(g_eid2));
    Say(line);
    UE_LOGI("%s host %s said ready: eid %u", kTag, row.name, Wire(g_eid));
    HEnter(HStep::AwaitDone);
}

void ResetStep() {
    g_target.Reset();
    g_target2.Reset();
    g_eid = g_eid2 = g_kerfurId = EL::kInvalidId;
    g_turnedOff = false;
}

void HostTick(coop::net::Session* s, const Phase& ph) {
    if (g_hstep == HStep::Done) return;
    const uint64_t now = NowMs();
    if (now < g_hNextMs) return;
    g_hNextMs = now + kPollEveryMs;
    if (g_hstep != HStep::Arm && !s->HasPeerConn(kClientSlot)) {
        HostFail(nullptr, "the client left before the verdict");
        return;
    }
    const Row& row = ph.rows[g_hrow < ph.count ? g_hrow : 0];
    switch (g_hstep) {
    case HStep::Arm:
        // The client's join and its late window are over, so what is spawned reaches it as a live spawn.
        if (!s->IsSlotWorldReady(kClientSlot) || !coop::prop_snapshot::IsBracketClosed(kClientSlot) ||
            coop::join_window_baseline::IsLateWindowOpen(kClientSlot) || !coop::players::Registry::Get().Local())
            return;
        UE_LOGI("%s host %s armed on slot %d", kTag, ph.name, kClientSlot);
        HEnter(HStep::Spawn);
        return;
    case HStep::Spawn: {
        if (row.target == Target::Client) {
            // The client makes this step's target; the counters are taken now and the client acts on the line.
            SayReady(row);
            return;
        }
        void* actor = nullptr;
        if (const char* why = SpawnTarget(row, 0.f, &actor)) {
            HostFail(&row, why);
            return;
        }
        g_target.Set(actor);
        if (row.target == Target::HostTwo) {
            void* second = nullptr;
            if (const char* why = SpawnTarget(row, kPairGapCm, &second)) {
                HostFail(&row, why);
                return;
            }
            g_target2.Set(second);
        }
        HEnter(HStep::Announce);
        return;
    }
    case HStep::Announce:
        if (row.target == Target::HostKerfur && !KerfurPropReady(row)) return;
        if (!TargetAnnounced(row, g_target, &g_eid)) return;
        if (row.target == Target::HostTwo && !TargetAnnounced(row, g_target2, &g_eid2)) return;
        SayReady(row);
        return;
    case HStep::AwaitDone: {
        std::string rest;
        if (!ClientSaid(DoneHead(row), &rest)) {
            if (HExpired(kDoneBoundMs)) HostFail(&row, "the client never said it was done");
            return;
        }
        if (rest != "ok") {
            HostFail(&row, "the client reported: " + rest);
            return;
        }
        HEnter(HStep::Judge);
        return;
    }
    case HStep::Judge: {
        std::string why;
        switch (ph.judge(row, g_eid, g_base, &why)) {
        case Judged::Wait:
            if (HExpired(kJudgeBoundMs)) HostFail(&row, "still waiting: " + why);
            return;
        case Judged::Fail:
            HostFail(&row, why);
            return;
        case Judged::Pass:
            UE_LOGI("%s host %s step %s PASS -- %s", kTag, ph.name, row.name, why.c_str());
            ResetStep();
            if (++g_hrow < ph.count) {
                HEnter(HStep::Spawn);
                return;
            }
            UE_LOGI("%s host %s PASS -- all %u steps held", kTag, ph.name, static_cast<unsigned>(ph.count));
            g_hstep = HStep::Done;
            return;
        }
        return;
    }
    case HStep::Done:
        return;
    }
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

}  // namespace

void Install(coop::net::Session* s) {
    if (!s || PhaseName() == "off") return;
    if (!ActivePhase()) {
        UE_LOGW("%s phase %s has no steps in this build -- nothing will run", kTag, PhaseName().c_str());
        return;
    }
    UE_LOGI("%s armed: phase %s on the %s", kTag, PhaseName().c_str(),
            s->role() == coop::net::Role::Host ? "host" : "client");
}

void Tick(coop::net::Session* s) {
    const Phase* const ph = ActivePhase();
    if (!ph || !s) return;
    if (s->role() == coop::net::Role::Host) HostTick(s, *ph);
    else if (s->connected()) ClientTick(s, *ph);
}

void OnDisconnect() {
    g_hstep = HStep::Arm;
    g_hStepMs = g_hNextMs = 0;
    g_hrow = 0;
    ResetStep();
    g_base = Baseline{};
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

}  // namespace coop::dev::prop_act_drill
