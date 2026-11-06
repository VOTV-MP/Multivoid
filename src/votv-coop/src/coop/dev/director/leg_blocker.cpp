// coop/dev/director/leg_blocker.cpp -- see coop/dev/director/leg_blocker.h.

#include "coop/dev/director/leg_blocker.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"

#include <cmath>
#include <string>

namespace coop::director {
namespace {
namespace R = ue_wrap::reflection;

constexpr float kKneeAboveCentreCm = 50.f;   // the walker's knee line, under its centre (BlockerAhead's)
constexpr float kFloorProbeCm      = 300.f;  // the floor is traced down this far under the walker

}  // namespace

bool IsPhysicsProp(void* actor) {
    return actor && ue_wrap::prop::IsDescendantOfProp(actor) && !ue_wrap::prop::IsFrozen(actor) &&
           !ue_wrap::prop::IsStatic(actor);
}

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
    const bool isProp = IsPhysicsProp(hit.actor);
    UE_LOGW("director/Goto:   the route's leg %zu (%.0f,%.0f,%.0f)->(%.0f,%.0f,%.0f) hits '%ls' (%ls) at "
            "(%.0f,%.0f,%.0f), %.0f cm along -- %s", heading, a.X, a.Y, a.Z, b.X, b.Y, b.Z, actor.c_str(),
            actorCls.c_str(), hit.point.X, hit.point.Y, hit.point.Z, std::hypot(hit.point.X - a.X, hit.point.Y - a.Y),
            isProp ? "a physics prop, the blocker" : "not a prop");
    if (!isProp) return false;
    *prop = hit.actor;
    *spot = hit.point;
    return true;
}

}  // namespace coop::director
