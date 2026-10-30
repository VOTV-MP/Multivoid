// coop/dev/container_intent_drill_throw_into_stand.cpp -- where the throwinto client stands to throw (see the header): never a straight-line
// guess, never a point that only projects onto the navmesh, a point the director's own route rule reaches, found from a fan round the target.

#include "container_intent_drill_throw_into_stand.h"

#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/routes.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace coop::dev::container_intent_drill::detail::into {

namespace E  = ue_wrap::engine;
namespace DR = coop::director;

namespace {
constexpr float kAnyReachCm = 1.0e6f;   // a reach no route end misses: asks only whether a route exists at all
}  // namespace

void TallyText(const StandTally& t, char* buf, size_t len) {
    std::snprintf(buf, len, "%d projected, of those %d without a route, %d with a route ending short, %d with a route that winds", t.projected,
                  t.noRoute, t.endsShort, t.winds);
}

RouteVerdict RouteTo(void* ctx, const FVector& from, const FVector& to, float reachCm, float* len) {
    float length = 0.f;
    if (!DR::RouteInLegs(ctx, from, to, reachCm, &length))   // a route that ends short is told from none by asking again with any end accepted
        return DR::RouteInLegs(ctx, from, to, kAnyReachCm) ? RouteVerdict::EndsShort : RouteVerdict::NoRoute;
    const float straight = std::hypot(to.X - from.X, to.Y - from.Y);
    if (length > kRouteWindFactor * straight + kRouteWindExtraCm) return RouteVerdict::Winds;
    if (len) *len = length;
    return RouteVerdict::Ok;
}

bool FindStand(void* player, void* pack, int r, int t, float cm, float reachCm, FVector& out, StandTally& tally) {
    FVector origin{}, extent{}, pAt{};
    if (!pack || !E::GetActorBounds(pack, false, origin, extent) || !E::TryGetActorLocation(player, pAt)) return false;
    const float eyeZ = E::GetCameraLocation().Z;
    const FVector mouth{origin.X, origin.Y, origin.Z + extent.Z + 5.f};
    const float base = std::atan2(pAt.Y - origin.Y, pAt.X - origin.X);
    float bestLen = 0.f;
    int bestDir = -1;
    for (int i = 0; i < kFan; ++i) {
        const FVector want = FanPoint(FVector{origin.X, origin.Y, pAt.Z}, base, i, cm);
        FVector proj{};
        if (!NavSnap(player, want, &proj)) continue;
        if (std::fabs(proj.X - origin.X) <= extent.X && std::fabs(proj.Y - origin.Y) <= extent.Y) continue;   // inside the basket
        // The held drive and the thrower's own pawn are not in the way of the line.
        void* ignore[2] = {DR::Grabbing(player), player};
        const uint8_t types[2] = {0, 1};   // WorldStatic, WorldDynamic: the set LineBlockedStatDyn traces
        ue_wrap::trace::Hit hit;
        if (!ue_wrap::trace::LineHitForObjects(player, FVector{proj.X, proj.Y, eyeZ}, mouth, types, 2, ignore + (ignore[0] ? 0 : 1),
                                               ignore[0] ? 2 : 1, &hit))
            continue;
        if (hit.blocked && hit.actor != pack) continue;   // another actor is in the way; the mouth is on the basket itself
        ++tally.projected;
        float len = 0.f;
        const RouteVerdict v = RouteTo(player, pAt, proj, reachCm, &len);
        if (v == RouteVerdict::NoRoute) { ++tally.noRoute; continue; }
        if (v == RouteVerdict::EndsShort) { ++tally.endsShort; continue; }
        if (v == RouteVerdict::Winds) { ++tally.winds; continue; }
        if (bestDir >= 0 && len >= bestLen) continue;
        bestLen = len;
        bestDir = i;
        out = {proj.X, proj.Y, pAt.Z};
    }
    if (bestDir < 0) return false;
    UE_LOGI("[CI-DRILL] client %s n=%d try %d: standpoint (%.0f,%.0f,%.0f), fan direction %d, %.0f cm from the basket, route %.0f cm from the player "
            "(%.0f cm straight); fan so far: %d projected, %d without a route, %d ending short, %d winding", ArmName(), r, t,
            static_cast<double>(out.X), static_cast<double>(out.Y), static_cast<double>(out.Z), bestDir, static_cast<double>(cm),
            static_cast<double>(bestLen), static_cast<double>(std::hypot(out.X - pAt.X, out.Y - pAt.Y)), tally.projected, tally.noRoute,
            tally.endsShort, tally.winds);
    return true;
}

// The control's hold loop corrects the camera-to-point distance by grabLen anyway.
bool FindStandWide(void* player, void* pack, int r, int t, float cm, float reachCm, FVector& out, StandTally& tally) {
    if (FindStand(player, pack, r, t, cm, reachCm, out, tally)) return true;
    for (const float wide : {kIntoWidenCm, kIntoWiden2Cm})
        if (cm < wide && FindStand(player, pack, r, t, wide, reachCm, out, tally)) return true;
    return false;
}

bool WantAfterMiss(const FVector& stand, const FVector& packAt, const FVector& rest, float lo, float hi, float& cm) {
    float ux = 0.f, uy = 0.f, len = 0.f;
    if (!Unit(stand, packAt, ux, uy, len)) return false;
    const float along = (rest.X - packAt.X) * ux + (rest.Y - packAt.Y) * uy;
    cm = std::clamp(len + along, lo, hi);
    return true;
}

}  // namespace coop::dev::container_intent_drill::detail::into
