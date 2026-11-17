// coop/dev/end_trace.cpp -- see coop/dev/end_trace.h.

#include "coop/dev/end_trace.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

#include <chrono>
#include <cmath>
#include <vector>

namespace coop::dev::end_trace {
namespace {

namespace E = ue_wrap::engine;
using ue_wrap::FVector;

constexpr int kTraceTicks = 2;    // the ticks traced right after the End's sleep
constexpr int kLaterTicks = 180;  // about three seconds on: where it rests
constexpr size_t kMaxPending = 8;

struct Pending {
    ue_wrap::CachedObjRef actor;
    uint32_t eid = 0;
    uint8_t gen = 0;
    FVector endPose{};
    bool slept = false;
    int tick = 0;
};
std::vector<Pending> g_pending;

// The open-row trace: the last line said per eid, and where the body stood then.
struct RowSeen {
    uint32_t eid = 0;
    std::chrono::steady_clock::time_point at{};
    FVector pose{};
};
std::vector<RowSeen> g_rowSeen;

bool OpenRowTraceOn() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::open_row_trace);
    return on;
}

}  // namespace

bool IsEnabled() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::end_trace);
    return on;
}

void OnEndApplied(void* actor, uint32_t eid, uint8_t gen, const FVector& endPose, bool slept) {
    if (!IsEnabled() || !actor) return;
    for (Pending& p : g_pending) {
        if (p.eid != eid) continue;
        p.actor.Set(actor);
        p.gen = gen;
        p.endPose = endPose;
        p.slept = slept;
        p.tick = 0;
        return;
    }
    if (g_pending.size() >= kMaxPending) return;
    Pending p;
    p.actor.Set(actor);
    p.eid = eid;
    p.gen = gen;
    p.endPose = endPose;
    p.slept = slept;
    g_pending.push_back(std::move(p));
}

void NoteOpenRow(const char* who, uint32_t eid, uint8_t gen, void* actor) {
    if (!OpenRowTraceOn() || !actor) return;
    const auto now = std::chrono::steady_clock::now();
    RowSeen* seen = nullptr;
    for (RowSeen& r : g_rowSeen)
        if (r.eid == eid) seen = &r;
    if (seen && now - seen->at < std::chrono::seconds(1)) return;
    FVector at{};
    if (!E::TryGetActorLocation(actor, at)) return;
    const float delta = seen ? std::hypot(std::hypot(at.X - seen->pose.X, at.Y - seen->pose.Y), at.Z - seen->pose.Z) : 0.f;
    UE_LOGI("[ROW-TRACE] %s eid=%u gen=%u: the author's body awake=%d, moved %.2f cm since the last line (z %.2f)", who, eid,
            static_cast<unsigned>(gen), E::ReadActorRootBodyAwake(actor), static_cast<double>(delta), static_cast<double>(at.Z));
    if (!seen) {
        if (g_rowSeen.size() >= 16) g_rowSeen.erase(g_rowSeen.begin());
        g_rowSeen.push_back(RowSeen{eid, now, at});
    } else {
        seen->at = now;
        seen->pose = at;
    }
}

void Tick() {
    if (!IsEnabled() || g_pending.empty()) return;
    for (size_t i = 0; i < g_pending.size();) {
        Pending& p = g_pending[i];
        void* actor = p.actor.Get();
        if (!actor) { g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
        FVector at{};
        const bool read = E::TryGetActorLocation(actor, at);
        if (p.tick < kTraceTicks) {
            const ue_wrap::prop::VelocityState v = ue_wrap::prop::GetPhysicsVelocity(actor);
            FVector origin{}, extent{};
            float bottom = 0.f, floorZ = 0.f;
            bool floor = false;
            if (E::GetActorBounds(actor, /*onlyColliding=*/true, origin, extent)) {
                bottom = origin.Z - extent.Z;
                ue_wrap::trace::Hit hit;
                floor = ue_wrap::trace::LineHitStatDyn(actor, FVector{origin.X, origin.Y, bottom + 30.f},
                                                       FVector{origin.X, origin.Y, bottom - 100.f}, &hit) && hit.blocked;
                if (floor) floorZ = hit.point.Z;
            }
            UE_LOGI("[END-TRACE] eid=%u gen=%u tick %d (sleep call ran %d): awake=%d lin=(%.2f, %.2f, %.2f) ang=(%.2f, %.2f, %.2f) "
                    "bottom z %.2f floor z %s%.2f penetration %.2f cm, z %.2f against the End's %.2f", p.eid,
                    static_cast<unsigned>(p.gen), p.tick, p.slept ? 1 : 0, E::IsActorRootBodyAtRest(actor) ? 0 : 1,
                    v.linearCmS.X, v.linearCmS.Y, v.linearCmS.Z, v.angularDegS.X, v.angularDegS.Y, v.angularDegS.Z, bottom,
                    floor ? "" : "(none within 130 cm) ", floorZ, floor ? floorZ - bottom : 0.f, read ? at.Z : 0.f, p.endPose.Z);
        } else if (p.tick == kLaterTicks) {
            UE_LOGI("[END-TRACE] eid=%u gen=%u later (tick %d): awake=%d z now %.2f against the End's %.2f (%+.2f cm), %.2f cm from it",
                    p.eid, static_cast<unsigned>(p.gen), p.tick, E::IsActorRootBodyAtRest(actor) ? 0 : 1,
                    read ? at.Z : 0.f, p.endPose.Z, read ? at.Z - p.endPose.Z : 0.f,
                    read ? std::hypot(std::hypot(at.X - p.endPose.X, at.Y - p.endPose.Y), at.Z - p.endPose.Z) : 0.f);
            g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++p.tick;
        ++i;
    }
}

}  // namespace coop::dev::end_trace
