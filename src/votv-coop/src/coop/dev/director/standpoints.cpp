// coop/dev/director/standpoints.cpp -- see coop/dev/director/standpoints.h.

#include "coop/dev/director/standpoints.h"

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace coop::director {
namespace {

namespace E = ue_wrap::engine;

float Flat(const ue_wrap::FVector& a, const ue_wrap::FVector& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

constexpr int   kMaxLegs         = 16;     // a walk across the map is a handful of partial routes
constexpr float kMinLegProgressCm = 100.f;  // a leg that ends no nearer than this has found the edge of the reachable

}  // namespace

bool RouteInLegs(void* player, const ue_wrap::FVector& from, const ue_wrap::FVector& to, float reachCm,
                 float* outLength) {
    ue_wrap::FVector cur = from;
    float len = 0.f;
    for (int leg = 0; leg < kMaxLegs; ++leg) {
        std::vector<ue_wrap::FVector> route;
        if (!E::FindNavPath(player, cur, to, route) || route.empty()) return false;
        for (size_t k = 1; k < route.size(); ++k) len += Flat(route[k - 1], route[k]);
        const ue_wrap::FVector end = route.back();
        if (Flat(end, to) <= reachCm) {
            if (outLength) *outLength = len;
            return true;
        }
        if (Flat(end, to) > Flat(cur, to) - kMinLegProgressCm) return false;   // no nearer: unreachable from here
        cur = end;
    }
    return false;
}

std::vector<ue_wrap::FVector> ReachableStandpoints(void* player, const ue_wrap::FVector& about, float ringCm, int count,
                                                   float reachCm) {
    ue_wrap::FVector from;
    if (!player || count <= 0 || !E::TryGetActorLocation(player, from)) return {};
    std::vector<std::pair<float, ue_wrap::FVector>> found;
    for (int i = 0; i < count; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(count);
        ue_wrap::FVector p = from;  // at the height the player stands at
        p.X = about.X + ringCm * std::cos(a);
        p.Y = about.Y + ringCm * std::sin(a);
        float len = 0.f;
        if (!RouteInLegs(player, from, p, reachCm, &len)) continue;
        found.emplace_back(len, p);
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    std::vector<ue_wrap::FVector> out;
    for (const auto& f : found) out.push_back(f.second);
    return out;
}

}  // namespace coop::director
