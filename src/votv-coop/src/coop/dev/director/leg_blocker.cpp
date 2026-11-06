// coop/dev/director/leg_blocker.cpp -- see coop/dev/director/leg_blocker.h.

#include "coop/dev/director/leg_blocker.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace coop::director {
namespace {
namespace R = ue_wrap::reflection;

constexpr float kKneeAboveCentreCm = 50.f;   // the walker's knee line, under its centre (BlockerAhead's)
constexpr float kFloorProbeCm      = 300.f;  // the floor is traced down this far under the walker

}  // namespace

bool LegBlocker(const PlayerContext& ctx, const std::vector<ue_wrap::FVector>& route, size_t heading, void** prop,
                ue_wrap::FVector* spot) {
    if (heading == 0 || heading >= route.size()) return false;
    // The route's points sit on the floor: the leg is traced at the walker's own knee height above it.
    ue_wrap::trace::Hit under;
    if (!ue_wrap::trace::LineHitStatDyn(ctx.player, ctx.pos, {ctx.pos.X, ctx.pos.Y, ctx.pos.Z - kFloorProbeCm}, &under) ||
        !under.blocked) {
        UE_LOGW("director/Goto:   the floor under the walker is unread -- the route's leg is not traced");
        return false;
    }
    const float lift = (ctx.pos.Z - kKneeAboveCentreCm) - under.point.Z;
    const ue_wrap::FVector a{route[heading - 1].X, route[heading - 1].Y, route[heading - 1].Z + lift};
    const ue_wrap::FVector b{route[heading].X, route[heading].Y, route[heading].Z + lift};
    ue_wrap::trace::Hit hit;
    if (!ue_wrap::trace::LineHitStatDyn(ctx.player, a, b, &hit)) {
        UE_LOGW("director/Goto:   the route's leg %zu trace unresolved", heading);
        return false;
    }
    if (!hit.blocked) {
        UE_LOGW("director/Goto:   the route's leg %zu (%.0f,%.0f,%.0f)->(%.0f,%.0f,%.0f) is clear", heading, a.X, a.Y, a.Z,
                b.X, b.Y, b.Z);
        return false;
    }
    const std::wstring actor = hit.actor ? R::ToString(R::NameOf(hit.actor)) : L"?";
    const std::wstring actorCls = hit.actor ? R::ClassNameOf(hit.actor) : L"?";
    const bool isProp = hit.actor && ue_wrap::prop::IsDescendantOfProp(hit.actor);
    UE_LOGW("director/Goto:   the route's leg %zu (%.0f,%.0f,%.0f)->(%.0f,%.0f,%.0f) hits '%ls' (%ls) at "
            "(%.0f,%.0f,%.0f), %.0f cm along -- %s", heading, a.X, a.Y, a.Z, b.X, b.Y, b.Z, actor.c_str(),
            actorCls.c_str(), hit.point.X, hit.point.Y, hit.point.Z, std::hypot(hit.point.X - a.X, hit.point.Y - a.Y),
            isProp ? "a prop, the blocker" : "not a prop");
    if (!isProp) return false;
    *prop = hit.actor;
    *spot = hit.point;
    return true;
}

Hold PropHoldsWalker(const PlayerContext& ctx, const std::vector<ue_wrap::FVector>& route, size_t heading, void* prop,
                     float reachCm, float* gapCm) {
    if (!prop || heading == 0 || heading >= route.size()) return Hold::Unread;
    float dx = route[heading].X - route[heading - 1].X, dy = route[heading].Y - route[heading - 1].Y;
    const float len = std::hypot(dx, dy);
    const float radius = ue_wrap::engine::GetActorCharacterRadius(ctx.player);
    ue_wrap::FVector centre{}, ext{};
    // A zero extent is "no colliding components", never a point-sized prop (engine.cpp, ActorReachSphere).
    if (len < 1.f || radius <= 0.f || !ue_wrap::engine::GetActorBounds(prop, /*onlyColliding=*/true, centre, ext) ||
        (ext.X == 0.f && ext.Y == 0.f))
        return Hold::Unread;
    dx /= len;
    dy /= len;
    // The box's closest point to the walker's centre, flat; the body's surface is the capsule radius short of it.
    const float nx = (std::max)(std::fabs(ctx.pos.X - centre.X) - ext.X, 0.f);
    const float ny = (std::max)(std::fabs(ctx.pos.Y - centre.Y) - ext.Y, 0.f);
    const float gap = (std::max)(std::hypot(nx, ny) - radius, 0.f);
    if (gapCm) *gapCm = gap;
    // Ahead: some corner of the box lies in front of the walker along the leg.
    bool ahead = false;
    for (int sx = -1; sx <= 1 && !ahead; sx += 2)
        for (int sy = -1; sy <= 1 && !ahead; sy += 2)
            ahead = (centre.X + sx * ext.X - ctx.pos.X) * dx + (centre.Y + sy * ext.Y - ctx.pos.Y) * dy > 0.f;
    if (!ahead) return Hold::Behind;
    return gap <= reachCm ? Hold::Holds : Hold::TooFar;
}

}  // namespace coop::director
