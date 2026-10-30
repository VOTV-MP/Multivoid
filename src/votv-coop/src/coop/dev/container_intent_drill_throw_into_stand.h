// coop/dev/container_intent_drill_throw_into_stand.h -- where the throwinto client stands to throw: a standpoint round the target that a
// NavMesh route reaches, and the distance a miss moves it to. A private header of coop/dev/container_intent_drill_throw_into*.cpp (src tree,
// not include/). See container_intent_drill_throw_into_stand.cpp.

#pragma once

#include "container_intent_drill_throw_into.h"

#include <cstddef>
#include <cstdint>

namespace coop::dev::container_intent_drill::detail::into {

inline constexpr float kStandReachCm       = 60.f;    // the director's walk ends this near its standpoint
inline constexpr float kRouteWindFactor   = 1.5f;    // a route longer than this x the straight distance ...
inline constexpr float kRouteWindExtraCm  = 200.f;   // ... plus this winds round something and is refused

// What the fans of one pick refused, summed over the calls that fill it (a fresh tally per pick): `projected` fan points projected to the
// navmesh and passed the basket and line checks; of those, `noRoute` have no route from the player at all, `endsShort` a route whose last
// point is past the walk's reach of the point, `winds` a route longer than the wind limit above.
struct StandTally {
    int projected = 0, noRoute = 0, endsShort = 0, winds = 0;
};
// "<projected> projected, of those <n> without a route, <n> with a route ending short, <n> with a route that winds" into `buf`.
void TallyText(const StandTally& t, char* buf, size_t len);

// The director's rule for a walked-to point (PickReachablePile): a NavMesh route from `from`, asked in legs, whose last point lies within
// `reachCm` of `to` and which is not longer than the wind limit. Ok sets `len`, the route's flat length, when given.
enum class RouteVerdict : uint8_t { Ok, NoRoute, EndsShort, Winds };
RouteVerdict RouteTo(void* ctx, const FVector& from, const FVector& to, float reachCm, float* len = nullptr);

// A standpoint `cm` from `pack` that the player can walk to by a route: a fan of kFan directions round it (the player's side first), each
// wanted point projected to the navmesh and kept only outside the basket's bounds, with a clear line from eye height to its mouth, and a
// RouteTo from the player that ends within `reachCm`; the shortest route among those kept wins. `tally` counts the refusals; `r` and `t`
// name the try in the line it prints.
bool FindStand(void* player, void* pack, int r, int t, float cm, float reachCm, FVector& out, StandTally& tally);
// The same, widened: `cm` first, then kIntoWidenCm, then kIntoWiden2Cm (those farther than `cm`).
bool FindStandWide(void* player, void* pack, int r, int t, float cm, float reachCm, FVector& out, StandTally& tally);
// The distance from the basket at `packAt` the thrown standpoint `stand` moves to after a miss of a rest at `rest`, along the line from
// the standpoint to the basket, kept in [lo, hi].
bool WantAfterMiss(const FVector& stand, const FVector& packAt, const FVector& rest, float lo, float hi, float& cm);

}  // namespace coop::dev::container_intent_drill::detail::into
