// coop/dev/container_intent_drill_throw_into_spawn.cpp -- where the throwinto host places its baskets and drives (see the header).

#include "container_intent_drill_throw_into_spawn.h"
#include "container_intent_drill_throw_into_stand.h"
#include "container_route_drill_detail.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace coop::dev::container_intent_drill::detail::into {
namespace {

namespace E  = ue_wrap::engine;
namespace ci = ue_wrap::container_inventory;
namespace RD = coop::dev::container_route_drill::detail;

constexpr float kSpawnLiftCm = 5.f;   // a target spawns this far above the floor

}  // namespace

FVector Place(const FVector& at, const FVector& fwd, float ahead, float side) {
    return {at.X + fwd.X * ahead + fwd.Y * side, at.Y + fwd.Y * ahead - fwd.X * side, at.Z + RD::kDropCm};
}

void* SpawnBasket(const FVector& where, const FVector& faceTo) {
    const ue_wrap::FRotator rot{0.f, std::atan2(faceTo.Y - where.Y, faceTo.X - where.X) * (180.f / 3.14159265358979f), 0.f};
    void* cls = ue_wrap::asset_load::LoadObjectByPath(kBasketPath);
    void* target = cls ? E::BeginDeferredSpawn(cls, where, rot) : nullptr;
    if (target && !E::FinishDeferredSpawn(target, where, rot)) target = nullptr;   // a failed finish destroys the actor
    if (target && ci::IsContainer(target)) return target;
    if (target) E::DestroyActor(target);
    UE_LOGW("[CI-DRILL] host %s: the basket did not load or is not a container (class %p, actor %p)", ArmName(), cls, target);
    return nullptr;
}

// How many of the kFan points `cm` round `c` project to the navmesh and have a route from `from` (RouteTo, the walk's reach), and the mean
// height of those that do (the floor).
static int RoutedRing(void* ctx, const FVector& from, const FVector& c, float cm, float* meanZ) {
    int n = 0;
    float sumZ = 0.f;
    for (int i = 0; i < kFan; ++i) {
        FVector p{};
        if (!NavSnap(ctx, FanPoint(c, 0.f, i, cm), &p)) continue;
        if (RouteTo(ctx, from, p, kStandReachCm) != RouteVerdict::Ok) continue;
        ++n;
        sumZ += p.Z;
    }
    *meanZ = n > 0 ? sumZ / static_cast<float>(n) : c.Z;
    return n;
}

bool RingSpot(void* ctx, const FVector& from, const FVector& first, float sx, float sy, int minValid, FVector& out, char* why, size_t whyLen) {
    int best = 0;
    for (int k = 0; k <= kRingSteps; ++k) {
        const FVector c{first.X + sx * kRingStepCm * static_cast<float>(k), first.Y + sy * kRingStepCm * static_cast<float>(k), first.Z};
        float floorZ = c.Z;
        const int valid = RoutedRing(ctx, from, c, kRingCm, &floorZ);
        best = (std::max)(best, valid);
        if (valid < minValid) continue;
        out = {c.X, c.Y, floorZ + kSpawnLiftCm};
        UE_LOGI("[CI-DRILL] host %s: the target's spot (%.0f,%.0f,%.0f) after %d step(s) has %d of %d navmesh points with a route from the client %.0f cm round it", ArmName(),
                static_cast<double>(out.X), static_cast<double>(out.Y), static_cast<double>(out.Z), k, valid, kFan, static_cast<double>(kRingCm));
        return true;
    }
    std::snprintf(why, whyLen, "no spot with a routed ring of navmesh round it within %d step(s) of %.0f cm (best %d of %d points with a route, %d needed)",
                  kRingSteps, static_cast<double>(kRingStepCm), best, kFan, minValid);
    return false;
}

bool RingSpotFan(void* ctx, const FVector& from, const FVector& at, const FVector& fwd, float ahead, float side, bool stepSide, int minValid, FVector& out,
                 char* why, size_t whyLen) {
    char last[240] = "";
    for (int k = 0; k < kFan; ++k) {
        const float turn = static_cast<float>((k + 1) / 2) * (6.28318531f / static_cast<float>(kFan)) * (k % 2 == 1 ? 1.f : -1.f);
        const FVector d{fwd.X * std::cos(turn) - fwd.Y * std::sin(turn), fwd.X * std::sin(turn) + fwd.Y * std::cos(turn), 0.f};
        if (RingSpot(ctx, from, Place(at, d, ahead, side), stepSide ? d.Y : d.X, stepSide ? -d.X : d.Y, minValid, out, last, sizeof(last))) {
            UE_LOGI("[CI-DRILL] host %s: the spot is in fan direction %d of %d (turned %.0f degrees from the heading)", ArmName(), k, kFan,
                    static_cast<double>(turn * (180.f / 3.14159265358979f)));
            return true;
        }
    }
    std::snprintf(why, whyLen, "%s in any of %d directions round at=(%.0f,%.0f,%.0f) fwd=(%.2f,%.2f); last direction: %s", "no spot with a routed ring of navmesh",
                  kFan, static_cast<double>(at.X), static_cast<double>(at.Y), static_cast<double>(at.Z), static_cast<double>(fwd.X),
                  static_cast<double>(fwd.Y), last);
    return false;
}

}  // namespace coop::dev::container_intent_drill::detail::into
