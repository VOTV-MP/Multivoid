// coop/dev/director/routes.cpp -- see coop/dev/director/routes.h.

#include "coop/dev/director/routes.h"

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace coop::director {
namespace {

namespace E = ue_wrap::engine;

float Flat(const ue_wrap::FVector& a, const ue_wrap::FVector& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

constexpr int   kMaxLegs         = 16;     // a walk across the map is a handful of partial routes
// RouteFrom's boxes for the NavMesh about an off-mesh start, each way on all three axes (so a storey within
// one counts): the near one, then a wide one for a walker the game's own physics put far off it.
constexpr float kProjectCm[]     = {400.f, 2000.f};
constexpr float kMinLegProgressCm = 100.f;  // a leg that ends no nearer than this has found the edge of the reachable

// Legs from `from` toward `to` until one ends within `nearCm` of it. On true, `lastFrom` is where that
// leg started, `lenBefore` the flat length walked before it and `lenAll` the whole; each may be null.
bool Legs(void* player, const ue_wrap::FVector& from, const ue_wrap::FVector& to, float nearCm,
          ue_wrap::FVector* lastFrom, float* lenBefore, float* lenAll) {
    ue_wrap::FVector cur = from;
    float len = 0.f;
    for (int leg = 0; leg < kMaxLegs; ++leg) {
        std::vector<ue_wrap::FVector> route;
        if (!RouteFrom(player, cur, to, &route)) return false;
        float legLen = 0.f;
        for (size_t k = 1; k < route.size(); ++k) legLen += Flat(route[k - 1], route[k]);
        const ue_wrap::FVector end = route.back();
        if (Flat(end, to) <= nearCm) {
            if (lastFrom) *lastFrom = cur;
            if (lenBefore) *lenBefore = len;
            if (lenAll) *lenAll = len + legLen;
            return true;
        }
        if (Flat(end, to) > Flat(cur, to) - kMinLegProgressCm) return false;   // no nearer: unreachable from here
        len += legLen;
        cur = end;
    }
    return false;
}

}  // namespace

bool RouteFrom(void* player, const ue_wrap::FVector& from, const ue_wrap::FVector& to,
               std::vector<ue_wrap::FVector>* route) {
    route->assign(1, from);
    std::vector<ue_wrap::FVector> path;
    if (!E::FindNavPath(player, from, to, path)) {
        ue_wrap::FVector onMesh{};
        bool routed = false;
        for (const float box : kProjectCm) {
            if (!E::ProjectToNav(player, from, {box, box, box}, &onMesh)) continue;
            // A start the projection hands back where it was is on the mesh already: the query failed on
            // the far end, and asking again from the same point would fail the same way.
            if (std::fabs(onMesh.X - from.X) < 1.f && std::fabs(onMesh.Y - from.Y) < 1.f &&
                std::fabs(onMesh.Z - from.Z) < 1.f)
                return false;
            routed = E::FindNavPath(player, onMesh, to, path);
            break;
        }
        if (!routed) return false;
        route->push_back(onMesh);
    }
    route->insert(route->end(), path.begin() + 1, path.end());   // the query's own first point is its start
    return true;
}

bool RouteInLegs(void* player, const ue_wrap::FVector& from, const ue_wrap::FVector& to, float reachCm,
                 float* outLength) {
    return Legs(player, from, to, reachCm, nullptr, nullptr, outLength);
}

std::vector<ue_wrap::FVector> ReachableStandpoints(void* player, const ue_wrap::FVector& about, float ringCm, int count,
                                                   float reachCm) {
    ue_wrap::FVector from;
    if (!player || count <= 0 || !E::TryGetActorLocation(player, from)) return {};
    // The legs toward the ring's centre are walked once, to the leg whose route reaches the ring: every
    // point is asked from that leg's start, as a walker reaches the ring from there whichever point it
    // takes, so a far ring costs its legs once and not once per point.
    const ue_wrap::FVector centre{about.X, about.Y, from.Z};   // at the height the player stands at
    ue_wrap::FVector start{};
    float before = 0.f;
    bool legged = Legs(player, from, centre, ringCm + reachCm, &start, &before, nullptr);
    // A centre off the NavMesh (a desk's button, a port) may end no route: walk the legs toward the ring's
    // points instead, the first that reaches lending its last leg's start to the rest.
    for (int i = 0; !legged && i < count; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(count);
        const ue_wrap::FVector p{about.X + ringCm * std::cos(a), about.Y + ringCm * std::sin(a), from.Z};
        legged = Legs(player, from, p, reachCm, &start, &before, nullptr);
    }
    if (!legged) return {};
    std::vector<std::pair<float, ue_wrap::FVector>> found;
    for (int i = 0; i < count; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(count);
        const ue_wrap::FVector p{about.X + ringCm * std::cos(a), about.Y + ringCm * std::sin(a), from.Z};
        std::vector<ue_wrap::FVector> route;
        if (!RouteFrom(player, start, p, &route) || Flat(route.back(), p) > reachCm) continue;
        float len = before;
        for (size_t k = 1; k < route.size(); ++k) len += Flat(route[k - 1], route[k]);
        found.emplace_back(len, p);
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    std::vector<ue_wrap::FVector> out;
    for (const auto& f : found) out.push_back(f.second);
    return out;
}

}  // namespace coop::director
