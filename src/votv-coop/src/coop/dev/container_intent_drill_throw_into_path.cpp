// coop/dev/container_intent_drill_throw_into_path.cpp -- what a thrown copy's path did against the target, for the throwinto arm:
// measured from the thrower's own samples, never guessed. One line per thrown try, printed by the client once the copy settled:
// the closest approach of the path (the samples joined by segments) to the trigger's centre and its time after the release,
// whether that point and any sample lay inside the target's bounds box, and the launch velocity from the first samples.

#include "container_intent_drill_throw_into.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace coop::dev::container_intent_drill::detail::into {
namespace {

bool Inside(const FVector& p, const FVector& origin, const FVector& extent) {
    return std::fabs(p.X - origin.X) <= extent.X && std::fabs(p.Y - origin.Y) <= extent.Y && std::fabs(p.Z - origin.Z) <= extent.Z;
}

}  // namespace

void PathPrint(const char* role, int n, int t, const Track& tr, uint64_t pressMs, const FVector& trigger, const FVector& origin,
               const FVector& extent) {
    const std::vector<Sample>& s = tr.samples;
    if (s.empty()) {
        UE_LOGI("[CI-DRILL] %s %s n=%d try %d path: no sample of the thrown copy", role, ArmName(), n, t);
        return;
    }
    float best = Dist3(s[0].pos, trigger);
    double bestT = static_cast<double>(s[0].t);
    FVector bestP = s[0].pos;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        const FVector& a = s[i].pos;
        const float abx = s[i + 1].pos.X - a.X, aby = s[i + 1].pos.Y - a.Y, abz = s[i + 1].pos.Z - a.Z;
        const float len2 = abx * abx + aby * aby + abz * abz;
        float u = 0.f;
        if (len2 > 0.f) u = std::clamp(((trigger.X - a.X) * abx + (trigger.Y - a.Y) * aby + (trigger.Z - a.Z) * abz) / len2, 0.f, 1.f);
        const FVector p{a.X + abx * u, a.Y + aby * u, a.Z + abz * u};
        const float d = Dist3(p, trigger);
        if (d >= best) continue;
        best = d;
        bestP = p;
        bestT = static_cast<double>(s[i].t) + static_cast<double>(u) * static_cast<double>(s[i + 1].t - s[i].t);
    }
    int inside = 0;
    for (const Sample& smp : s)
        if (Inside(smp.pos, origin, extent)) ++inside;
    // The launch velocity: the first sample against the first one at least kSpeedWindowMs later.
    FVector v{};
    uint64_t dt = 0;
    for (size_t i = 1; i < s.size(); ++i)
        if (s[i].t >= s[0].t + kSpeedWindowMs) {
            dt = s[i].t - s[0].t;
            v = {(s[i].pos.X - s[0].pos.X) * 1000.f / static_cast<float>(dt), (s[i].pos.Y - s[0].pos.Y) * 1000.f / static_cast<float>(dt),
                 (s[i].pos.Z - s[0].pos.Z) * 1000.f / static_cast<float>(dt)};
            break;
        }
    UE_LOGI("[CI-DRILL] %s %s n=%d try %d path: closest approach to the trigger's centre (%.0f,%.0f,%.0f) is %.1f cm at (%.0f,%.0f,%.0f), "
            "%.0f ms after the release, that point %s the bounds box; %d of %zu samples inside the box; launch velocity "
            "(%.0f,%.0f,%.0f) cm/s = %.0f cm/s over the first %llu ms",
            role, ArmName(), n, t, static_cast<double>(trigger.X), static_cast<double>(trigger.Y), static_cast<double>(trigger.Z),
            static_cast<double>(best), static_cast<double>(bestP.X), static_cast<double>(bestP.Y), static_cast<double>(bestP.Z),
            bestT - static_cast<double>(pressMs), Inside(bestP, origin, extent) ? "inside" : "outside", inside, s.size(),
            static_cast<double>(v.X), static_cast<double>(v.Y), static_cast<double>(v.Z),
            static_cast<double>(std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z)), U(dt));
}

}  // namespace coop::dev::container_intent_drill::detail::into
