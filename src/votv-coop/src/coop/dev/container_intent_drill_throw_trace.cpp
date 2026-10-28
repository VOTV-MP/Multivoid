// coop/dev/container_intent_drill_throw_trace.cpp -- one actor's trace on one peer, for the container intent drill's throw
// arms; see container_intent_drill_throw.h. Lines are tagged [CI-DRILL]:
//   throw <role> <which> eid=<n> t=<UnixMs> pos=(x,y,z) vel=(x,y,z) sim=<0|1> drivenBy=<slot|-1|self> [rtt=<ms>]
// drivenBy names the peer slot whose pose stream moves the actor, `self` when this peer's own player or driven channel holds
// it, -1 when nothing does.

#include "container_intent_drill_throw.h"

#include "coop/dev/container_route_census.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"   // IsActorRootSimulatingPhysics

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace coop::dev::container_intent_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace cen = coop::dev::container_route_census;
using ue_wrap::FVector;

constexpr size_t kMaxSamples = 1800;   // 30 s at 60 fps
constexpr int    kFirstLines = 30;
constexpr int    kEveryNth   = 5;
constexpr int    kMaxLines   = 200;
constexpr int    kRttEvery   = 20;

void DrivenBy(void* actor, char* out, size_t cap) {
    if (!coop::remote_prop::IsActorUnderAnyDrive(actor)) {
        std::snprintf(out, cap, "%s", coop::remote_prop::IsOwnedHere(actor) ? "self" : "-1");
        return;
    }
    for (int s = 0; s < coop::net::kMaxPeers; ++s)
        if (!coop::remote_prop::IsActorDrivenByOtherThan(actor, s)) {
            std::snprintf(out, cap, "%d", s);
            return;
        }
    std::snprintf(out, cap, "mixed");   // two slots drive it
}

float Horiz(const FVector& a, const FVector& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

}  // namespace

float Dist3(const FVector& a, const FVector& b) {
    return std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));
}

void TrackBegin(Track& t, const char* role, const char* which) {
    t = Track{};
    t.role = role;
    t.which = which;
    t.samples.reserve(kMaxSamples);
}

void TrackSet(Track& t, void* actor) {
    t.ref.Set(actor);
    t.on = actor != nullptr;
    t.eid = actor ? static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(actor)) : 0u;
    if (t.eid == static_cast<uint32_t>(coop::element::kInvalidId)) t.eid = 0;
}

void TrackTick(Track& t, int rttMs) {
    if (!t.on) return;
    const uint64_t now = cen::UnixMs();
    void* actor = t.ref.Get();
    FVector at{};
    if (!actor || !E::TryGetActorLocation(actor, at)) {
        if (!t.gone) {
            t.gone = true;
            t.goneMs = now;
            UE_LOGI("[CI-DRILL] throw %s %s eid=%u gone t=%llu", t.role, t.which, t.eid, static_cast<unsigned long long>(now));
        }
        return;
    }
    if (t.eid == 0)   // an adopted copy gets its element id a tick or more after its birth
        t.eid = static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(actor));
    if (t.eid == static_cast<uint32_t>(coop::element::kInvalidId)) t.eid = 0;
    if (t.samples.empty()) {
        t.first = at;
        t.restRef = at;
        t.restRefMs = now;
    }
    // The speed: the displacement against the oldest sample at least kSpeedWindowMs old.
    float speed = 0.f;
    while (t.window + 1 < t.samples.size() && t.samples[t.window + 1].t + kSpeedWindowMs <= now) ++t.window;
    if (!t.samples.empty() && now >= t.samples[t.window].t + kSpeedWindowMs) {
        const Sample& ref = t.samples[t.window];
        speed = Dist3(at, ref.pos) * 1000.f / static_cast<float>(now - ref.t);
    }
    if (t.samples.size() < kMaxSamples) t.samples.push_back(Sample{now, at, speed});
    t.last = at;
    t.maxHoriz = std::max(t.maxHoriz, Horiz(at, t.first));
    if (speed > t.peakSpeed) {
        t.peakSpeed = speed;
        t.peakT = now;
        t.peakPos = at;
    }
    if (Dist3(at, t.restRef) >= kRestMoveCm) {
        t.restRef = at;
        t.restRefMs = now;
        t.atRest = false;
        t.restMs = 0;
    } else if (!t.atRest && now - t.restRefMs >= kRestWindowMs) {
        t.atRest = true;
        t.restMs = t.restRefMs;
    }
    ++t.seen;
    if ((t.seen > kFirstLines && t.seen % kEveryNth != 0) || t.lines >= kMaxLines) return;
    ++t.lines;
    const FVector v = E::GetActorVelocity(actor);
    char driven[12];
    DrivenBy(actor, driven, sizeof(driven));
    char rtt[16] = "";
    if (t.lines == 1 || t.lines % kRttEvery == 0) std::snprintf(rtt, sizeof(rtt), " rtt=%d", rttMs);
    UE_LOGI("[CI-DRILL] throw %s %s eid=%u t=%llu pos=(%.1f,%.1f,%.1f) vel=(%.1f,%.1f,%.1f) sim=%d drivenBy=%s%s", t.role,
            t.which, t.eid, static_cast<unsigned long long>(now), at.X, at.Y, at.Z, v.X, v.Y, v.Z,
            E::IsActorRootSimulatingPhysics(actor) ? 1 : 0, driven, rtt);
}

const char* ArcOf(const Track& t) {
    if (t.samples.empty()) return "none";
    return t.maxHoriz >= kFlewCm ? "flew" : "fell";
}

const Sample* NearestSample(const Track& t, const FVector& pos, float& gapCm) {
    const Sample* best = nullptr;
    gapCm = -1.f;
    for (const Sample& s : t.samples) {
        const float d = Dist3(s.pos, pos);
        if (!best || d < gapCm) {
            best = &s;
            gapCm = d;
        }
    }
    return best;
}

}  // namespace coop::dev::container_intent_drill::detail
