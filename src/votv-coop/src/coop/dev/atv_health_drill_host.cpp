// coop/dev/atv_health_drill_host.cpp -- the ATV health drill's arms where the HOST acts, `occupied` and `synced`: see
// coop/dev/atv_health_drill.h. The host walks to the ATV it spawned with the director, takes its seat (the game's own
// verb) or grabs it (the grav hand), says the edge in the lobby chat, and then judges the client's raw request for the
// same seat or the same ATV: the lane's refusal counter for it must rise by one and nothing may be granted. The
// client's side of both arms is atv_health_drill_arms.cpp.

#include "atv_health_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/dev/drill_chat.h"
#include "coop/interactables/atv_authority.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace coop::dev::atv_health_drill::detail {
namespace {

namespace A  = ue_wrap::atv;
namespace AU = coop::atv_authority;
namespace DC = coop::dev::drill_chat;
namespace E  = ue_wrap::engine;
using coop::atv_sync::TableView;
using coop::net::AtvRefusal;

// The refusal this arm waits for.
AtvRefusal WantRefusal(Arm a) { return a == Arm::Occupied ? AtvRefusal::Occupied : AtvRefusal::Synced; }

uint64_t RefusedSincePark(AtvRefusal r) {
    const unsigned i = static_cast<unsigned>(r) & 15u;
    return AU::ReadCounters().refused[i] - g_h.base.refused[i];
}

// Every other refusal since the park: what a request refused for another reason would show.
uint64_t OtherRefusedSincePark(AtvRefusal r) {
    uint64_t n = 0;
    for (unsigned i = 1; i < 16; ++i) n += AU::ReadCounters().refused[i] - g_h.base.refused[i];
    return n - RefusedSincePark(r);
}

uint64_t AdmittedSinceAct() { return AU::ReadCounters().admitted - g_h.snap.admitted; }

void HostFail(Arm a, const std::string& what, const std::string& got, const std::string& want) {
    Fail("host", a, g_h.run, what, got, want);
    HostEnd();
}

// A bound that ran out with no event to decide on.
void HostInvalid(Arm a, const std::string& what, const std::string& got, const std::string& want) {
    Invalid("host", a, g_h.run, what, got, want);
    HostEnd();
}

// The director's walk to the ATV's root, started once the table holds it; the host then stands within reach.
void Walk(Arm a, const TableView* t) {
    if (!g_h.walk) {
        ue_wrap::FVector at{};
        ue_wrap::FRotator rot{};
        if (t && A::GetRootTransform(t->actor, at, rot)) {
            const float reach = a == Arm::Synced ? kGrabReachCm : kSitReachCm;
            g_h.walk = coop::director::StartBackgroundWalk(at, reach, kWalkDeadlineS);
            g_h.walkAt = Clock::now();
            UE_LOGI("[ATV-HEALTH-DRILL] host arm=%s the ATV is at (%.0f,%.0f,%.0f), walking to it", SpecOf(a).name,
                    at.X, at.Y, at.Z);
        } else if (Past(g_h.run, kHostFirstBound)) {
            HostInvalid(a, "the ATV's table never held", "no ATV", "ATV indexed");
        }
        return;
    }
    if (!t) return HostFail(a, "the ATV is gone before the walk ended", "no ATV", "the ATV");
    const int state = g_h.walk->state.load();
    if (state == 2) return HostFail(a, "the director's walk to the ATV did not arrive", "walk failed", "arrived");
    if (state != 1) {
        if (Past(g_h.run, kHostFirstBound)) HostInvalid(a, "the director's walk never ended", "walking", "arrived");
        return;
    }
    g_h.arrivedAt = Clock::now();
    void* player = coop::players::Registry::Get().Local();
    if (a == Arm::Synced) g_h.grab = std::make_unique<coop::director::AimedGrab>(player, t->actor);
    HostEnter(HP::Act, a == Arm::Synced ? "grab" : "sit");
}

// The host's own seat (through the game's verb) or grab, then the table it judged for itself: the seat or the carry
// is the host's, the syncer stays the host. The lane's counters are snapshotted and the edge said.
void Act(Arm a, const TableView* t) {
    if (!t) return HostFail(a, "the ATV is gone before the act", "no ATV", "the ATV");
    void* player = coop::players::Registry::Get().Local();
    const bool sit = a == Arm::Occupied;
    if (sit) {
        if (!atv_seat::TrySit(player, t->actor, g_h.sit)) {
            if (Past(g_h.run, kFailBound)) HostInvalid(a, "the host's sit never finished", "no grant", "seated");
            return;
        }
        if (t->occupantSlot != 0 || t->syncerSlot != 0) {
            char got[48];
            std::snprintf(got, sizeof(got), "occupant %u syncer %u", static_cast<unsigned>(t->occupantSlot),
                          static_cast<unsigned>(t->syncerSlot));
            return HostFail(a, "the host's sit did not take", got, "occupant 0 syncer 0");
        }
    } else {
        const coop::director::GrabState st = g_h.grab->Tick();
        if (st == coop::director::GrabState::Working) {
            if (Past(g_h.run, kFailBound)) HostInvalid(a, "the host's grab never finished", "working", "grabbed");
            return;
        }
        if (st == coop::director::GrabState::Failed) return HostFail(a, GrabWhy(*g_h.grab), "failed", "grabbed");
        g_h.grab.reset();
        if (t->syncerSlot != 0)
            return HostFail(a, "the host's grab did not leave the host the syncer",
                            "syncer " + std::to_string(t->syncerSlot), "syncer 0");
    }
    g_h.snap = AU::ReadCounters();
    g_h.firstAt = Clock::now();
    DC::Say(PhaseLine(SpecOf(a).first));
    HostEnter(HP::Judge, "judge");
}

// From the client's `sent` line, every tick: its request refused for the arm's reason exactly once and nothing granted
// since the host's act (and, for the carry, the syncer still the host). A grant ends the judgement at once, since it
// cannot be undone; anything else waits for the bound. The host then undoes its act.
void Judge(Arm a, const TableView* t) {
    if (!t) return HostFail(a, "the ATV is gone before the judgement", "no ATV", "the ATV");
    if (!g_h.sawSent && DC::PollDue()) g_h.sawSent = DC::ChatHasLive(ClientLine("sent"));
    if (!g_h.sawSent) {
        if (Past(g_h.run, kFailBound))
            HostInvalid(a, "the client never sent its raw request", "no line", ClientLine("sent"));
        return;
    }
    const AtvRefusal want = WantRefusal(a);
    const uint64_t refused = RefusedSincePark(want);
    const uint64_t granted = AdmittedSinceAct();
    if (granted != 0)
        return HostFail(a, "the host's judgement granted the client's request", "admitted " + Plus(granted),
                        "admitted +0");
    const bool syncerKept = a != Arm::Synced || t->syncerSlot == 0;
    if (refused == 1 && syncerKept) {
        DC::Say(PhaseLine("back"));
        char nums[64];
        std::snprintf(nums, sizeof(nums), "walk_ms=%lld", Ms(g_h.walkAt, g_h.arrivedAt));
        Pass("host", a, g_h.run, nums);
        void* player = coop::players::Registry::Get().Local();
        if (a == Arm::Occupied) { if (A::IsDriven(t->actor)) A::Dismount(t->actor); }
        else E::ReleaseMainPlayerGrabIfHolding(player, t->actor);
        return HostEnd();
    }
    // The lane's refusals are the events: more than one, or the one with the carry no longer the host's, is the FAIL at
    // once; none by the bound says INVALID, the request having never been judged.
    const std::string got = "refused " + Plus(refused) + ", others " + Plus(OtherRefusedSincePark(want)) + ", syncer " +
                            std::to_string(t->syncerSlot);
    if (refused > 1) return HostFail(a, "the request was refused more than once", got, "refused +1, admitted +0, syncer 0");
    if (refused == 1) return HostFail(a, "the request was refused but the carry left the host", got, "refused +1, admitted +0, syncer 0");
    if (Past(g_h.run, kFailBound))
        HostInvalid(a, "the client's request was never judged", got, "refused +1, admitted +0, syncer 0");
}

}  // namespace

void ActHostTick(Arm a, const TableView* t) {
    switch (static_cast<HP>(g_h.run.phase)) {
    case HP::Walk:  return Walk(a, t);
    case HP::Act:   return Act(a, t);
    case HP::Judge: return Judge(a, t);
    default:        return;
    }
}

void HostEnd() {
    if (g_h.walk && g_h.walk->state.load() == 0) coop::director::CancelBackgroundWalk(*g_h.walk);
    g_h.walk.reset();
    g_h.grab.reset();
}

}  // namespace coop::dev::atv_health_drill::detail
