// coop/dev/prop_act_drill.cpp -- see coop/dev/prop_act_drill.h.

#include "coop/dev/prop_act_drill.h"

#include "coop/comms/chat_log.h"
#include "coop/comms/chat_sync.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/kerfur_entity.h"
#include "coop/dev/drill_props.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_act_host.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_snapshot.h"
#include "coop/props/remote_prop.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/actors/prop_flags.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

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
std::string DoneHead(const Row& r, int slot) { return std::string(kTag) + " c" + std::to_string(slot) + " " + r.name + " done "; }

void Say(const std::string& line) { coop::chat_sync::QueueSend(line); }

}  // namespace detail

namespace {

namespace E   = ue_wrap::engine;
namespace EL  = coop::element;
namespace PAH = coop::prop_act_host;
namespace KE  = coop::kerfur_entity;
namespace PRP = coop::remote_prop;
namespace DP  = coop::dev::drill_props;
using ue_wrap::FVector;

constexpr const wchar_t* kKerfurNpc = L"kerfurOmega_C";

// Once a session: the session start calls Install many times, and EdgeInstall sets the client's announce hold.
bool g_installed = false;

// The host's spawn: the puppet's facing, then the rest of a fan round it, the first direction and distance whose
// column has a floor (found by a trace).
constexpr float kAheadCm[] = {200.f, 150.f, 250.f};
constexpr float kFanDeg[] = {0.f, 45.f, -45.f, 90.f, -90.f, 135.f, -135.f, 180.f};
constexpr int kFanCount = static_cast<int>(sizeof(kFanDeg) / sizeof(kFanDeg[0]));
int g_fanDir = -1;                       // the fan index a step's first target took; its pair's second reads it
constexpr float kDropCm = 40.f;          // above the floor, so the prop settles
constexpr float kFloorDownCm = 400.f;    // how far under the puppet's middle the floor is looked for
constexpr float kMinColumnCm = 20.f;     // a column whose floor is nearer than this to its start began inside geometry
constexpr float kNpcLiftCm = 60.f;       // a kerfur NPC is placed above the floor so its capsule clears it

const Phase* ActivePhase() {
    static const Phase* const p = PhaseName() == "honest" ? &Honest() : nullptr;
    return p;
}

// The race, depart and edge phases have no row table for the core: each runs its peers by itself.
bool RaceArmed() {
    static const bool v = PhaseName() == "race";
    return v;
}
bool DepartArmed() {
    static const bool v = PhaseName() == "depart";
    return v;
}
bool EdgeArmed() {
    static const bool v = PhaseName() == "edge";
    return v;
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

// The counters of the client's slot as the step begins, and the watch on its raw PropDestroy messages naming
// the step's props.
Baseline TakeBaseline(EL::ElementId eid, EL::ElementId eid2) {
    PRP::dev::WatchWireDestroys(kClientSlot, eid, eid2);
    Baseline b;
    b.destroys = PAH::CommitsOf(kClientSlot, PAH::internal::CommitStep::Destroy);
    b.aborts = PAH::CommitsOf(kClientSlot, PAH::internal::CommitStep::Abort);
    b.eid2 = eid2;
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
        if (!DP::TurnOffKerfur(npc)) {
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
    g_base = TakeBaseline(g_eid, g_eid2);
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
            coop::join_window_baseline::IsLateWindowOpen(kClientSlot) || !coop::players::Registry::Get().Local() ||
            !ClientPuppetReady())
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
        if (!ClientSaid(kClientSlot, DoneHead(row), &rest)) {
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

namespace detail {

// The text after `head` in the first row of the lobby record that `slot` said.
bool ClientSaid(int slot, const std::string& head, std::string* rest) {
    bool found = false;
    coop::chat_log::ForEach([&](const coop::chat_log::Row& r) {
        if (found || r.slot != slot || r.text.compare(0, head.size(), head) != 0) return;
        found = true;
        if (rest) *rest = r.text.substr(head.size());
    });
    return found;
}

// True once slot 1's puppet is live and has a pose, the place and facing SpawnTarget reads. The HOST arm gates wait on it.
bool ClientPuppetReady() {
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(kClientSlot);
    return rp.valid() && rp.HasPose();
}

bool MirrorGatherable(void* mirror, Verb verb) {
    return mirror && !coop::prop_drive_stream::IsParked(mirror) &&
           (verb != Verb::Pocket || ue_wrap::prop::IsGatherable(mirror));
}

std::string MirrorState(void* mirror) {
    if (!mirror) return " (no mirror)";
    char buf[96];
    std::snprintf(buf, sizeof buf, " (child=%d frozen=%d simulating=%d parked=%d)", E::IsChildActor(mirror) ? 1 : 0,
                  ue_wrap::prop::IsFrozen(mirror) ? 1 : 0, E::IsActorRootSimulatingPhysics(mirror) ? 1 : 0,
                  coop::prop_drive_stream::IsParked(mirror) ? 1 : 0);
    return buf;
}

// Spawns the row's class on the first floor found round slot 1's puppet, its facing first, then the rest of the fan;
// `extraCm` further along that direction when it is a pair's second, so the client's walk to the mirror is short. A
// kerfur row spawns the NPC. Null on success, else why not.
const char* SpawnTarget(const Row& row, float extraCm, void** out) {
    void* const player = coop::players::Registry::Get().Local();   // the floor trace's world context
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(kClientSlot);
    void* const puppet = rp.valid() ? rp.GetActor() : nullptr;
    FVector at{};
    if (!player) return "the host's player was not there";
    if (!puppet || !E::TryGetActorLocation(puppet, at)) return "slot 1's puppet place could not be read";
    FVector fwd = E::GetActorForwardVector(puppet);
    const float len = std::sqrt(fwd.X * fwd.X + fwd.Y * fwd.Y);
    if (len < 0.1f) return "slot 1's puppet facing could not be read";
    const bool npc = row.target == Target::HostKerfur;
    void* const cls = npc ? nullptr : ue_wrap::asset_load::LoadObjectByPath(row.path);
    if (!npc && !cls) return "the target's class did not load";
    // A step's first target (extraCm 0) searches the whole fan and keeps its direction; the pair's second reads it.
    const bool second = extraCm > 0.f && g_fanDir >= 0;
    for (int i = second ? g_fanDir : 0; i < (second ? g_fanDir + 1 : kFanCount); ++i) {
        const float rad = kFanDeg[i] * (3.14159265f / 180.f);
        const float dx = (fwd.X * std::cos(rad) - fwd.Y * std::sin(rad)) / len;
        const float dy = (fwd.X * std::sin(rad) + fwd.Y * std::cos(rad)) / len;
        for (const float base : kAheadCm) {
            const float ahead = base + extraCm;
            const float x = at.X + dx * ahead, y = at.Y + dy * ahead;
            float floorZ = 0.f;
            void* p = nullptr;
            const DP::Drop d = DP::DropOnFloor(player, cls, kKerfurNpc, x, y, at.Z, kFloorDownCm, kMinColumnCm,
                                               npc ? kDropCm + kNpcLiftCm : kDropCm, &p, &floorZ);
            if (d == DP::Drop::NoFloor) continue;
            if (d == DP::Drop::SpawnFailed) return "the target did not spawn";
            UE_LOGI("%s host %s spawned %ls at (%.0f, %.0f, %.0f), %.0f uu away at %.0f deg from slot 1's puppet's "
                    "facing", kTag, row.name, npc ? kKerfurNpc : row.leaf, x, y, floorZ + kDropCm, ahead, kFanDeg[i]);
            if (!second) g_fanDir = i;
            *out = p;
            return nullptr;
        }
    }
    if (!second) g_fanDir = -1;
    // Why the columns missed, read again at the facing's first column and under the puppet itself.
    static const char* const kMiss[] = {"floor", "trace did not run", "no hit", "starts inside geometry"};
    DP::FloorMiss facing = DP::FloorMiss::None, own = DP::FloorMiss::None;
    float z = 0.f;
    DP::FloorAt(player, at.X + fwd.X / len * kAheadCm[0], at.Y + fwd.Y / len * kAheadCm[0], at.Z, kFloorDownCm,
                kMinColumnCm, &z, &facing);
    DP::FloorAt(player, at.X, at.Y, at.Z, kFloorDownCm, kMinColumnCm, &z, &own);
    static char why[200];
    std::snprintf(why, sizeof why, "no floor was found round slot 1's puppet (puppet at %.0f %.0f %.0f; facing column: "
                  "%s; own column: %s)", at.X, at.Y, at.Z, kMiss[static_cast<int>(facing)], kMiss[static_cast<int>(own)]);
    return why;
}

}  // namespace detail

void Install(coop::net::Session* s) {
    if (!s || g_installed || PhaseName() == "off") return;
    if (!ActivePhase() && !RaceArmed() && !DepartArmed() && !EdgeArmed()) {
        g_installed = true;
        UE_LOGW("%s phase %s has no steps in this build -- nothing will run", kTag, PhaseName().c_str());
        return;
    }
    g_installed = true;
    UE_LOGI("%s armed: phase %s on the %s", kTag, PhaseName().c_str(),
            s->role() == coop::net::Role::Host ? "host" : "client");
    if (EdgeArmed()) EdgeInstall(s);
}

void Tick(coop::net::Session* s) {
    if (RaceArmed()) {
        if (!s) return;
        if (s->role() == coop::net::Role::Host) RaceHostTick(s);
        else if (s->connected()) RaceClientTick(s);
        return;
    }
    if (DepartArmed()) {
        if (!s) return;
        if (s->role() == coop::net::Role::Host) DepartHostTick(s);
        else if (s->connected()) DepartClientTick(s);
        return;
    }
    if (EdgeArmed()) {
        if (!s) return;
        if (s->role() == coop::net::Role::Host) EdgeHostTick(s);
        else EdgeClientTick(s);   // the window row runs before the link is announced
        return;
    }
    const Phase* const ph = ActivePhase();
    if (!ph || !s) return;
    if (s->role() == coop::net::Role::Host) HostTick(s, *ph);
    else if (s->connected()) ClientTick(s, *ph);
}

void OnDisconnect() {
    g_installed = false;
    g_hstep = HStep::Arm;
    g_hStepMs = g_hNextMs = 0;
    g_hrow = 0;
    ResetStep();
    g_base = Baseline{};
    ClientReset();
    RaceReset();
    DepartReset();
    EdgeReset();
}

}  // namespace coop::dev::prop_act_drill
