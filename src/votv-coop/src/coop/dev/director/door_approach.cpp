// coop/dev/director/door_approach.cpp -- see coop/dev/director/door_approach.h.

#include "coop/dev/director/door_approach.h"

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"

#include <cmath>
#include <utility>

namespace coop::director {
namespace {

namespace E = ue_wrap::engine;

float HorizDist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y;
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

bool PickDoorApproach(void* player, const ue_wrap::FVector& at, void* door, bool endsShortOk,
                      DoorApproach& best) {
    ue_wrap::FVector pos{};
    if (!door || !E::TryGetActorLocation(door, pos)) return false;
    const ue_wrap::FVector fwd = E::GetActorForwardVector(door);
    bool took = false;
    for (const float side : {1.f, -1.f}) {
        const ue_wrap::FVector target{pos.X + fwd.X * kDoorApproachCm * side, pos.Y + fwd.Y * kDoorApproachCm * side,
                                      pos.Z};
        std::vector<ue_wrap::FVector> path;
        if (!E::FindNavPath(player, at, target, path) || path.empty()) {
            ++best.noRoute;
            continue;
        }
        if (HorizDist(path.back(), target) > kRouteEndCm) {
            ++best.endsFar;
            if (!endsShortOk) continue;
        }
        float len = 0.f;
        for (size_t i = 1; i < path.size(); ++i) len += HorizDist(path[i - 1], path[i]);
        if (len >= best.len) continue;
        best.target = target;
        best.route = std::move(path);
        best.len = len;
        took = true;
    }
    return took;
}

}  // namespace coop::director
