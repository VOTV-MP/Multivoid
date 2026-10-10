// coop/dev/director/detour.h -- the way round a surface the walker's body cannot climb.
//
// The level's NavMesh is built for a steeper slope than the player walks (measured on a fresh New
// Game: the NavMesh 45.5 degrees, the player's floor limit 40.1), so a route may climb a flank the
// body slides off -- the walker stood 2 m short of a waypoint on a 44-degree slope for minutes, each
// fresh route leading back to the same spot. The NavMesh proposes and the body's own floor limit
// disposes: a walker stopped by a surface too steep to stand on remembers that spot and asks for a
// via point, one a route reaches over floor the body stands on and from which the route on to the
// goal passes no remembered spot. Dev director only; game thread.
#pragma once

#include "ue_wrap/core/types.h"

#include <cstddef>
#include <vector>

namespace coop::director::detour {

// Whether the floor under `at`, traced down from above it, is one the body stands on: its normal Z
// at least `floorZ`. True where nothing is hit, or the trace does not resolve, since a NavMesh point
// has floor and a doubt is no reason to refuse a route.
bool FloorStandable(void* player, const ue_wrap::FVector& at, float floorZ);

// Whether the way from `pos` toward `toward` is a slope too steep for the body: a knee-height trace,
// 2 m at most, hits a surface whose normal Z is under `floorZ` and flatter than a wall (a door, a box
// or a fence is the stuck handler's own). `spot` is where it hit.
bool SteepAhead(void* player, const ue_wrap::FVector& pos, const ue_wrap::FVector& toward, float floorZ,
                ue_wrap::FVector* spot);

// The floor ahead on the route the walker follows, checked before it gets there: a walker that went down
// into a gully stood at the foot of a wall too steep to climb, every way out of it climbing one, until the
// game killed it (a fresh New Game, twice). Each tick a few floor samples, about a stride apart, keep the
// next stretch of the route checked; a run of samples too steep for the body is the spot to go round,
// found while the walker can still go round it.
class Lookahead {
public:
    // A new route to check, from its first point.
    void Reset(const std::vector<ue_wrap::FVector>& route);
    // Checks up to a few samples, keeping the stretch ahead of `along` (the walker's distance along
    // the route) checked. True with `spot` when a run too steep for the body lies ahead of the walker.
    bool Advance(void* player, float along, float floorZ, ue_wrap::FVector* spot);
    // The walker's distance along the route, given the route point it heads for and its flat distance
    // to it.
    float Along(size_t toward, float toPoint) const;

private:
    std::vector<ue_wrap::FVector> route_;
    std::vector<float> cum_;   // the flat length to each route point
    float scanned_ = 0.f;      // checked up to here
    float steepRun_ = 0.f;     // the length of the run too steep so far, ending at scanned_
};

class Search {
public:
    // Remembers the spot `steep` and starts a search round it, from `from` toward `goal`. False, and no
    // search, round a spot whose last search was exhausted: the stuck handler grinds there instead.
    bool Begin(const ue_wrap::FVector& from, const ue_wrap::FVector& goal, const ue_wrap::FVector& steep,
               float floorZ);
    // Starts a search round a spot already remembered, the one a route passes (PassedSpot). Either entry,
    // round the spot the last way round was found for, says that way did not hold: its via point is
    // never offered again, so each retry tries the next way round.
    bool BeginRound(const ue_wrap::FVector& from, const ue_wrap::FVector& goal, size_t spot, float floorZ);

    enum class Step { Working, Found, Exhausted };
    // Candidates within a budget of floor samples per call, each a route query and its samples. On
    // Found, `route` is the way the search held: to the via point and on from it as far as the onward
    // route goes, its first point the walker's start. Exhausted: no candidate held.
    Step Advance(void* player, std::vector<ue_wrap::FVector>* route);

    // The remembered spot a route passes within clearance of, past its first stretch, or -1.
    int PassedSpot(const std::vector<ue_wrap::FVector>& route) const;

    bool Active() const { return active_; }
    // The via point of the last way round found.
    const ue_wrap::FVector& LastVia() const { return lastVia_; }
    size_t Spots() const { return spots_.size(); }

private:
    bool Holds(void* player, const ue_wrap::FVector& via, std::vector<ue_wrap::FVector>* walk, float* score);
    bool Standable(void* player, const ue_wrap::FVector& at);

    bool NearFailedVia(const ue_wrap::FVector& via) const;

    std::vector<ue_wrap::FVector> spots_;
    std::vector<bool> exhausted_;           // per spot: its last search held no way round
    std::vector<ue_wrap::FVector> failedVias_;
    ue_wrap::FVector from_{}, goal_{}, round_{};
    size_t roundSpot_ = 0;
    ue_wrap::FVector lastVia_{}, lastRound_{};
    bool  lastFound_ = false;               // the last search found a way round lastRound_, by lastVia_
    float floorZ_ = 0.f;
    bool  active_ = false;
    int   next_ = 0;                        // the next candidate: ring * kDirections + direction
    int   samples_ = 0;                     // engine calls (traces, route queries) spent in this Advance
    float bestScore_ = 0.f;
    std::vector<ue_wrap::FVector> best_;    // the best held route of the ring under evaluation
    ue_wrap::FVector bestVia_{};
};

}  // namespace coop::director::detour
