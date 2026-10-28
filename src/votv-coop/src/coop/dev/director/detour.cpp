// coop/dev/director/detour.cpp -- see detour.h.

#include "coop/dev/director/detour.h"

#include "coop/dev/director/routes.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/trace.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace coop::director::detour {
namespace {

constexpr float  kRingsCm[]     = {600.f, 1200.f, 2400.f, 4800.f};   // nearest first: a way round near the spot
constexpr int    kRings         = sizeof(kRingsCm) / sizeof(kRingsCm[0]);
constexpr int    kDirections    = 12;
constexpr int    kSampleBudget  = 120;      // engine calls per call: a candidate past it waits for the next tick
constexpr float  kMaxDetourRatio = 4.f;     // a way to the via point this many times its straight distance is no way round
constexpr int    kRouteCost     = 4;        // a route query counts as this many samples
constexpr float  kMinViaCm      = 300.f;    // a via point at the walker's feet is no way round
constexpr float  kSampleCm      = 100.f;    // floor samples along a route, about a stride apart
constexpr float  kSpotClearCm   = 300.f;    // a route keeps this far from a remembered spot
constexpr float  kSpotLevelCm   = 300.f;    // ...on its level: a spot on a slope below is not one above
constexpr float  kBackIntoMarginCm = 10.f;  // a route nearer the spot than its start by more than this runs back into it
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
constexpr float  kLookAheadCm   = 3000.f;   // the route is kept checked this far ahead of the walker
constexpr int    kLookPerTick   = 8;        // floor samples per tick for it
constexpr float  kSteepRunCm    = 200.f;    // a run this long too steep is a slope, not a bump or an edge
constexpr float  kLookMinAheadCm = 300.f;   // a spot nearer than this is the stuck handler's (SteepAhead)
constexpr float  kFailedViaCm   = 300.f;    // a candidate this near a via point that did not hold is not one

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

void Lookahead::Reset(const std::vector<ue_wrap::FVector>& route) {
    route_ = route;
    cum_.assign(route_.size(), 0.f);
    for (size_t k = 1; k < route_.size(); ++k) cum_[k] = cum_[k - 1] + Flat(route_[k - 1], route_[k]);
    scanned_ = 0.f;
    steepRun_ = 0.f;
}

float Lookahead::Along(size_t toward, float toPoint) const {
    if (toward >= cum_.size()) return cum_.empty() ? 0.f : cum_.back();
    return (std::max)(0.f, cum_[toward] - toPoint);
}

bool Lookahead::Advance(void* player, float along, float floorZ, ue_wrap::FVector* spot) {
    if (route_.size() < 2) return false;
    size_t seg = 1;
    for (int n = 0; n < kLookPerTick && scanned_ < cum_.back() && scanned_ < along + kLookAheadCm; ++n) {
        while (seg + 1 < cum_.size() && cum_[seg] < scanned_) ++seg;
        const float segLen = cum_[seg] - cum_[seg - 1];
        const float t = segLen > 0.f ? (scanned_ - cum_[seg - 1]) / segLen : 1.f;
        const ue_wrap::FVector a = route_[seg - 1], b = route_[seg];
        const ue_wrap::FVector p{a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t};
        steepRun_ = FloorStandable(player, p, floorZ) ? 0.f : steepRun_ + kSampleCm;
        scanned_ += kSampleCm;
        if (steepRun_ >= kSteepRunCm && scanned_ - kSteepRunCm > along + kLookMinAheadCm) {
            *spot = p;
            steepRun_ = 0.f;
            return true;
        }
    }
    return false;
}

bool Search::Begin(const ue_wrap::FVector& from, const ue_wrap::FVector& goal, const ue_wrap::FVector& spot,
                   float floorZ, SpotKind kind) {
    size_t at = spots_.size();
    for (size_t i = 0; i < spots_.size(); ++i)
        if (Flat(spots_[i], spot) <= kSpotClearCm && std::fabs(spots_[i].Z - spot.Z) <= kSpotLevelCm) at = i;
    if (at == spots_.size()) {
        if (spots_.size() == kMaxSpots) {
            spots_.erase(spots_.begin());
            exhausted_.erase(exhausted_.begin());
            kind_.erase(kind_.begin());
        }
        spots_.push_back(spot);
        exhausted_.push_back(false);
        kind_.push_back(kind);
        at = spots_.size() - 1;
    }
    return BeginRound(from, goal, at, floorZ);
}

bool Search::NearFailedVia(const ue_wrap::FVector& via) const {
    for (const ue_wrap::FVector& f : failedVias_)
        if (Flat(f, via) <= kFailedViaCm && std::fabs(f.Z - via.Z) <= kSpotLevelCm) return true;
    return false;
}

bool Search::BeginRound(const ue_wrap::FVector& from, const ue_wrap::FVector& goal, size_t spot, float floorZ) {
    if (exhausted_[spot]) return false;
    if (lastFound_ && Flat(spots_[spot], lastRound_) <= kSpotClearCm &&
        std::fabs(spots_[spot].Z - lastRound_.Z) <= kSpotLevelCm) {
        if (failedVias_.size() == kMaxSpots) failedVias_.erase(failedVias_.begin());
        failedVias_.push_back(lastVia_);
        UE_LOGI("director/detour: the way round by (%.0f,%.0f,%.0f) did not hold -- not offered again", lastVia_.X,
                lastVia_.Y, lastVia_.Z);
    }
    from_ = from;
    goal_ = goal;
    round_ = spots_[spot];
    roundSpot_ = spot;
    floorZ_ = floorZ;
    active_ = true;
    lastFound_ = false;
    failed_ = {};
    next_ = 0;
    best_.clear();
    bestScore_ = 0.f;
    UE_LOGI("director/detour: round the spot (%.0f,%.0f,%.0f), %s -- %zu remembered", round_.X, round_.Y, round_.Z,
            kind_[spot] == SpotKind::Slope ? "too steep for the body" : "blocking the body", spots_.size());
    return true;
}

namespace {

// The flat distance from `p` to the segment a-b, with `z` the segment's height at its closest point.
float SegmentFlatDist(const ue_wrap::FVector& a, const ue_wrap::FVector& b, const ue_wrap::FVector& p, float* z) {
    const float dx = b.X - a.X, dy = b.Y - a.Y;
    const float len2 = dx * dx + dy * dy;
    const float t = len2 > 0.f ? (std::max)(0.f, (std::min)(1.f, ((p.X - a.X) * dx + (p.Y - a.Y) * dy) / len2)) : 0.f;
    *z = a.Z + (b.Z - a.Z) * t;
    return std::hypot(p.X - (a.X + dx * t), p.Y - (a.Y + dy * t));
}

// Whether the route, which starts within kSpotClearCm of `spot`, runs back toward it over its first stretch: the closest
// approach of its segments (the stretch's first kSpotClearCm of flat length) is nearer than its first point by more
// than kBackIntoMarginCm. Segments, not sampled points: route points lie about a metre apart while the blocker stood
// 25-66 cm ahead, so a route through it can have no point near it.
bool RunsBackInto(const std::vector<ue_wrap::FVector>& route, const ue_wrap::FVector& spot) {
    const float start = Flat(spot, route.front());
    float along = 0.f;
    for (size_t k = 1; k < route.size() && along < kSpotClearCm; ++k) {
        const ue_wrap::FVector a = route[k - 1];
        ue_wrap::FVector b = route[k];
        const float seg = Flat(a, b);
        if (seg > kSpotClearCm - along) {   // only the stretch's own length
            const float t = (kSpotClearCm - along) / seg;
            b = {a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t};
        }
        float z = 0.f;
        const float d = SegmentFlatDist(a, b, spot, &z);
        if (std::fabs(spot.Z - z) <= kSpotLevelCm && d < start - kBackIntoMarginCm) return true;
        along += seg;
    }
    return false;
}

}  // namespace

// A spot the route starts within kSpotClearCm of is the one the walker stands at, so the clearance test cannot hold in
// the route's first stretch. For a blocker RunsBackInto judges that stretch instead (a route through a crate it stands
// beside); a slope's spot is skipped there, since every route from a start on the slope toward the goal comes nearer
// its spot. A route that starts farther out, and every route past its first stretch, keeps the plain clearance test
// over its length.
int Search::PassedSpotWhy(const std::vector<ue_wrap::FVector>& route, bool* backInto) const {
    if (backInto) *backInto = false;
    if (route.empty()) return -1;
    bool near[kMaxSpots] = {};
    for (size_t i = 0; i < spots_.size(); ++i) {
        near[i] = Flat(spots_[i], route.front()) <= kSpotClearCm &&
                  std::fabs(spots_[i].Z - route.front().Z) <= kSpotLevelCm;
        if (near[i] && kind_[i] == SpotKind::Blocker && RunsBackInto(route, spots_[i])) {
            if (backInto) *backInto = true;
            return static_cast<int>(i);
        }
    }
    int found = -1;
    Walk(route, [&](const ue_wrap::FVector& p, float along) {
        for (size_t i = 0; i < spots_.size(); ++i) {
            if (near[i] && along <= kSpotClearCm) continue;
            if (Flat(spots_[i], p) <= kSpotClearCm && std::fabs(spots_[i].Z - p.Z) <= kSpotLevelCm) {
                found = static_cast<int>(i);
                return false;
            }
        }
        return true;
    });
    return found;
}

bool Search::Standable(void* player, const ue_wrap::FVector& at) {
    ++samples_;
    return FloorStandable(player, at, floorZ_);
}

bool Search::Holds(void* player, const ue_wrap::FVector& via, std::vector<ue_wrap::FVector>* walk, float* score) {
    if (Flat(via, from_) < kMinViaCm) return false;
    if (NearFailedVia(via)) {
        ++failed_.failedVia;
        return false;
    }
    samples_ += kRouteCost;
    // The way there: floor the body stands on all along, clear of every remembered spot past the
    // stretch the walker stands in, and not running back toward one it starts beside.
    std::vector<ue_wrap::FVector> there;
    if (!RouteFrom(player, from_, via, &there) || there.size() < 2 || Flat(there.back(), via) > kViaReachCm) {
        ++failed_.route;
        return false;
    }
    bool back = false;
    if (PassedSpotWhy(there, &back) >= 0) {
        ++(back ? failed_.backInto : failed_.passes);
        return false;
    }
    if (Length(there) > kMaxDetourRatio * Flat(from_, via)) {
        ++failed_.tooLong;
        return false;
    }
    if (!Walk(there, [&](const ue_wrap::FVector& p, float) { return Standable(player, p); })) {
        ++failed_.floor;
        return false;
    }
    // The way on: clear of every remembered spot, its floor sampled for its first stretch. The walker
    // follows this very route, not a fresh one asked where it stands at the via point.
    std::vector<ue_wrap::FVector> on;
    samples_ += kRouteCost;
    if (!RouteFrom(player, via, goal_, &on) || on.size() < 2) {
        ++failed_.route;
        return false;
    }
    if (PassedSpotWhy(on, &back) >= 0) {
        ++(back ? failed_.backInto : failed_.passes);
        return false;
    }
    if (!Walk(on, [&](const ue_wrap::FVector& p, float along) { return along > kOnwardCheckCm || Standable(player, p); })) {
        ++failed_.steep;
        return false;
    }
    *score = Length(there) + Length(on) + Flat(on.back(), goal_);
    there.insert(there.end(), on.begin() + 1, on.end());
    *walk = std::move(there);
    return true;
}

void Search::Cancel() {
    if (!active_) return;
    active_ = false;
    failed_ = {};
    next_ = 0;
    best_.clear();
    bestScore_ = 0.f;
    UE_LOGI("director/detour: the search round (%.0f,%.0f,%.0f) was cancelled", round_.X, round_.Y, round_.Z);
}

Search::Step Search::Advance(void* player, std::vector<ue_wrap::FVector>* route) {
    if (!active_) return Step::Exhausted;
    samples_ = 0;
    for (int k = 0; next_ < kRings * kDirections && (k == 0 || samples_ < kSampleBudget); ++k, ++next_) {
        const int ring = next_ / kDirections, dir = next_ % kDirections;
        const float a = 6.28318530718f * static_cast<float>(dir) / static_cast<float>(kDirections);
        const ue_wrap::FVector c{round_.X + std::cos(a) * kRingsCm[ring], round_.Y + std::sin(a) * kRingsCm[ring],
                                 round_.Z};
        ue_wrap::FVector floor{};
        std::vector<ue_wrap::FVector> walk;
        float score = 0.f;
        ++samples_;
        const bool hasFloor = FloorUnder(player, c, &floor);
        if (!hasFloor) ++failed_.floor;
        if (hasFloor && Holds(player, {floor.X, floor.Y, floor.Z + kViaLiftCm}, &walk, &score) &&
            (best_.empty() || score < bestScore_)) {
            best_ = std::move(walk);
            bestScore_ = score;
            bestVia_ = {floor.X, floor.Y, floor.Z + kViaLiftCm};
        }
        if (dir == kDirections - 1 && !best_.empty()) {   // the best of the nearest ring that holds one
            active_ = false;
            lastFound_ = true;
            lastVia_ = bestVia_;
            lastRound_ = round_;
            UE_LOGI("director/detour: a via point %.0f cm round the spot holds, at (%.0f,%.0f,%.0f) (the held way %.0f "
                    "cm, %.0f cm to the goal in all)", kRingsCm[ring], bestVia_.X, bestVia_.Y, bestVia_.Z,
                    Length(best_), bestScore_);
            *route = std::move(best_);
            best_.clear();
            return Step::Found;
        }
    }
    if (next_ < kRings * kDirections) return Step::Working;
    active_ = false;
    // The spot may have shifted in the list (the oldest dropped at the cap); mark it only where it still is.
    if (roundSpot_ < exhausted_.size() && Flat(spots_[roundSpot_], round_) < 1.f) exhausted_[roundSpot_] = true;
    UE_LOGW("director/detour: no via point within %.0f cm of the spot holds (floor %d, route %d, long %d, failed-via %d, "
            "passes %d/back-into %d, steep %d) -- back to grinding", kRingsCm[kRings - 1], failed_.floor,
            failed_.route, failed_.tooLong, failed_.failedVia, failed_.passes, failed_.backInto, failed_.steep);
    return Step::Exhausted;
}

}  // namespace coop::director::detour
