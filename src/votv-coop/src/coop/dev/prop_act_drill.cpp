// coop/dev/prop_act_drill.cpp -- see coop/dev/prop_act_drill.h.

#include "coop/dev/prop_act_drill.h"

#include "coop/comms/chat_log.h"
#include "coop/comms/chat_sync.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/kerfur_entity.h"
#include "coop/dev/spawn_npc.h"
#include "coop/dispatch/event_feed.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_host.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::prop_act_drill {
using namespace detail;

namespace detail {

uint64_t NowMs() { return ::GetTickCount64(); }

const std::string& PhaseName() {
    static const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::prop_act_drill);
    return v;
}

// The lines the two peers say to each other in the lobby chat.
std::string ReadyHead(const Row& r) { return std::string(kTag) + " host " + r.name + " ready eid="; }
std::string DoneHead(const Row& r) { return std::string(kTag) + " c" + std::to_string(kClientSlot) + " " + r.name + " done "; }

void Say(const std::string& line) { coop::chat_sync::QueueSend(line); }

}  // namespace detail

namespace {

namespace E   = ue_wrap::engine;
namespace EL  = coop::element;
namespace PAH = coop::prop_act_host;
namespace KE  = coop::kerfur_entity;
using ue_wrap::FVector;

constexpr const wchar_t* kKerfurNpc = L"kerfurOmega_C";

// The host's spawn: along its player's facing, the first distance whose column has a floor; the floor found by a trace.
constexpr float kAheadCm[] = {200.f, 150.f, 250.f};
constexpr float kDropCm = 40.f;          // above the floor, so the prop settles
constexpr float kFloorDownCm = 400.f;    // how far under the player's middle the floor is looked for
constexpr float kMinColumnCm = 20.f;     // a column whose floor is nearer than this to its start began inside geometry
constexpr float kPairGapCm = 60.f;       // the second prop of a pair, further along the same line
constexpr float kNpcLiftCm = 60.f;       // a kerfur NPC is placed above the floor so its capsule clears it

// Bounds: each ends a step that did not get its line or its count, and FAILs it.
constexpr uint64_t kAnnounceBoundMs = 60000; // HOST: the spawn to the target having a row
constexpr uint64_t kDoneBoundMs = 180000;    // HOST: the ready line to the client's done line
constexpr uint64_t kJudgeBoundMs = 30000;    // HOST: the done line to the counters it must show

const Phase* ActivePhase() {
    static const Phase* const p = PhaseName() == "honest" ? &Honest() : nullptr;
    return p;
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
    ClientReset();
}

}  // namespace coop::dev::prop_act_drill
