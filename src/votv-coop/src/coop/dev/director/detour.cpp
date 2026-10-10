// coop/dev/director/detour.cpp -- see detour.h.

#include "coop/dev/director/detour.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine_nav.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace coop::director::detour {
namespace {

namespace E = ue_wrap::engine;

constexpr float  kRingsCm[]     = {600.f, 1200.f, 2400.f, 4800.f};   // nearest first: the shortest way round
constexpr int    kRings         = sizeof(kRingsCm) / sizeof(kRingsCm[0]);
constexpr int    kDirections    = 12;
constexpr int    kPerAdvance    = 2;        // candidates per call, a route query and its samples each
constexpr float  kSampleCm      = 100.f;    // floor samples along a route, about a stride apart
constexpr float  kSpotClearCm   = 300.f;    // a route keeps this far from a remembered spot
constexpr float  kSpotLevelCm   = 300.f;    // ...on its level: a spot on a slope below is not one above
constexpr float  kOnwardCheckCm = 1500.f;   // the onward route's floor is sampled this far past the via point
constexpr float  kViaReachCm    = 150.f;    // a route to the via point ends this near it
constexpr float  kProbeUpCm     = 150.f;    // FloorStandable's trace starts this far above the point...
constexpr float  kProbeDownCm   = 300.f;    // ...and ends this far below it
constexpr float  kDropCm        = 2000.f;   // a candidate is put on the floor within this above or below it
constexpr float  kViaLiftCm     = 50.f;     // ...and this far above that floor, inside the path query's reach
constexpr float  kTraceAheadCm  = 200.f;    // SteepAhead looks this far toward the waypoint at most
constexpr float  kKneeCm        = -50.f;    // ...at knee height, under the walker's centre
constexpr float  kWallNormalZ   = 0.3f;     // a hit flatter than a wall: a slope, not a door, box or fence
constexpr size_t kMaxSpots      = 32;

float Flat(const ue_wrap::FVector& a, const ue_wrap::FVector& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

float Length(const std::vector<ue_wrap::FVector>& r) {
    float len = 0.f;
    for (size_t k = 1; k < r.size(); ++k) len += Flat(r[k - 1], r[k]);
    return len;
}

// Calls `fn(point, along)` about every kSampleCm along the route, both ends included, `along` the
// flat distance from its start; stops at the first false and returns false.
template <class Fn>
bool Walk(const std::vector<ue_wrap::FVector>& r, Fn fn) {
    if (r.size() == 1) return fn(r[0], 0.f);
    float along = 0.f;
    for (size_t k = 1; k < r.size(); ++k) {
        const ue_wrap::FVector a = r[k - 1], b = r[k];
        const float seg = Flat(a, b);
        const int n = (std::max)(1, static_cast<int>(seg / kSampleCm));
        for (int i = (k == 1 ? 0 : 1); i <= n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            const ue_wrap::FVector p{a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t};
            if (!fn(p, along + seg * t)) return false;
        }
        along += seg;
    }
    return true;
}

// The floor under `p` within kDropCm above or below it; false when none is hit.
bool FloorUnder(void* player, const ue_wrap::FVector& p, ue_wrap::FVector* out) {
    ue_wrap::trace::Hit hit;
    if (!ue_wrap::trace::LineHitStatDyn(player, {p.X, p.Y, p.Z + kDropCm}, {p.X, p.Y, p.Z - kDropCm}, &hit) ||
        !hit.blocked)
        return false;
    *out = hit.point;
    return true;
}

}  // namespace

bool FloorStandable(void* player, const ue_wrap::FVector& at, float floorZ) {
    ue_wrap::trace::Hit hit;
    if (!ue_wrap::trace::LineHitStatDyn(player, {at.X, at.Y, at.Z + kProbeUpCm}, {at.X, at.Y, at.Z - kProbeDownCm},
                                        &hit) ||
        !hit.blocked)
        return true;
    return hit.normal.Z >= floorZ;
}

bool SteepAhead(void* player, const ue_wrap::FVector& pos, const ue_wrap::FVector& toward, float floorZ,
                ue_wrap::FVector* spot) {
    const float h = Flat(pos, toward);
    if (h < 1.f) return false;
    const float reach = (std::min)(h + 50.f, kTraceAheadCm);
    const ue_wrap::FVector a{pos.X, pos.Y, pos.Z + kKneeCm};
    const ue_wrap::FVector b{a.X + (toward.X - pos.X) / h * reach, a.Y + (toward.Y - pos.Y) / h * reach, a.Z};
    ue_wrap::trace::Hit hit;
    if (!ue_wrap::trace::LineHitStatDyn(player, a, b, &hit) || !hit.blocked) return false;
    if (hit.normal.Z >= floorZ || hit.normal.Z <= kWallNormalZ) return false;
    *spot = hit.point;
    return true;
}

void Search::Begin(const ue_wrap::FVector& from, const ue_wrap::FVector& goal, const ue_wrap::FVector& steep,
                   float floorZ) {
    size_t at = spots_.size();
    for (size_t i = 0; i < spots_.size(); ++i)
        if (Flat(spots_[i], steep) <= kSpotClearCm && std::fabs(spots_[i].Z - steep.Z) <= kSpotLevelCm) at = i;
    if (at == spots_.size()) {
        if (spots_.size() == kMaxSpots) spots_.erase(spots_.begin());
        spots_.push_back(steep);
        at = spots_.size() - 1;
    }
    BeginRound(from, goal, at, floorZ);
}

void Search::BeginRound(const ue_wrap::FVector& from, const ue_wrap::FVector& goal, size_t spot, float floorZ) {
    from_ = from;
    goal_ = goal;
    round_ = spots_[spot];
    floorZ_ = floorZ;
    active_ = true;
    next_ = 0;
    best_.clear();
    bestScore_ = 0.f;
    UE_LOGI("director/detour: round the spot (%.0f,%.0f,%.0f), too steep for the body -- %zu remembered", round_.X,
            round_.Y, round_.Z, spots_.size());
}

int Search::PassedSpot(const std::vector<ue_wrap::FVector>& route) const {
    int found = -1;
    Walk(route, [&](const ue_wrap::FVector& p, float along) {
        if (along <= kSpotClearCm) return true;
        for (size_t i = 0; i < spots_.size(); ++i)
            if (Flat(spots_[i], p) <= kSpotClearCm && std::fabs(spots_[i].Z - p.Z) <= kSpotLevelCm) {
                found = static_cast<int>(i);
                return false;
            }
        return true;
    });
    return found;
}

bool Search::Holds(void* player, const ue_wrap::FVector& via, std::vector<ue_wrap::FVector>* toVia,
                   float* score) const {
    // The way there: floor the body stands on all along, clear of every remembered spot past the
    // stretch the walker stands in.
    std::vector<ue_wrap::FVector> there;
    if (!E::FindNavPath(player, from_, via, there) || there.size() < 2 || Flat(there.back(), via) > kViaReachCm)
        return false;
    if (PassedSpot(there) >= 0) return false;
    if (!Walk(there, [&](const ue_wrap::FVector& p, float) { return FloorStandable(player, p, floorZ_); }))
        return false;
    // The way on: clear of every remembered spot, its floor sampled for its first stretch.
    std::vector<ue_wrap::FVector> on;
    if (!E::FindNavPath(player, via, goal_, on) || on.size() < 2) return false;
    if (PassedSpot(on) >= 0) return false;
    if (!Walk(on, [&](const ue_wrap::FVector& p, float along) {
            return along > kOnwardCheckCm || FloorStandable(player, p, floorZ_);
        }))
        return false;
    *score = Length(there) + Length(on) + Flat(on.back(), goal_);
    *toVia = std::move(there);
    return true;
}

Search::Step Search::Advance(void* player, std::vector<ue_wrap::FVector>* route) {
    if (!active_) return Step::Exhausted;
    for (int k = 0; k < kPerAdvance && next_ < kRings * kDirections; ++k, ++next_) {
        const int ring = next_ / kDirections, dir = next_ % kDirections;
        const float a = 6.28318530718f * static_cast<float>(dir) / static_cast<float>(kDirections);
        const ue_wrap::FVector c{round_.X + std::cos(a) * kRingsCm[ring], round_.Y + std::sin(a) * kRingsCm[ring],
                                 round_.Z};
        ue_wrap::FVector floor{};
        std::vector<ue_wrap::FVector> toVia;
        float score = 0.f;
        if (FloorUnder(player, c, &floor) &&
            Holds(player, {floor.X, floor.Y, floor.Z + kViaLiftCm}, &toVia, &score) &&
            (best_.empty() || score < bestScore_)) {
            best_ = std::move(toVia);
            bestScore_ = score;
        }
        if (dir == kDirections - 1 && !best_.empty()) {   // a ring done that held one: the nearest way round
            active_ = false;
            UE_LOGI("director/detour: a via point %.0f cm round the spot holds (route %.0f cm, %.0f cm to the goal "
                    "in all)", kRingsCm[ring], Length(best_), bestScore_);
            *route = std::move(best_);
            best_.clear();
            return Step::Found;
        }
    }
    if (next_ < kRings * kDirections) return Step::Working;
    active_ = false;
    UE_LOGW("director/detour: no via point within %.0f cm of the spot holds -- back to grinding",
            kRingsCm[kRings - 1]);
    return Step::Exhausted;
}

}  // namespace coop::director::detour
