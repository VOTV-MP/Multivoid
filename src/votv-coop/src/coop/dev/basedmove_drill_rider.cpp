// coop/dev/basedmove_drill_rider.cpp -- the local player walked onto the cargo lift's platform, for the based-movement drill; see
// coop/dev/basedmove_drill.h. Whichever peer rides runs this: it waits for its copy of the lift to rest, finds the platform's open
// side (basedmove_drill_sides.cpp), is walked by the director to the floor in front of it, steps in across the open edge onto the
// platform's surface through the input seam (a short straight leg: the NavMesh does not cover a moving platform), and says it
// boarded only when its feet stand on the surface. A leg that gains no ground ends the run INVALID at once.

#include "basedmove_drill_detail.h"

#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/cargo_lift.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::basedmove_drill::detail {

namespace E  = ue_wrap::engine;
namespace CL = ue_wrap::cargo_lift;

namespace {

constexpr float kReachCm = 60.f;       // the director's stop at a stand point beside the platform
constexpr float kStepReachCm = 30.f;   // the stop of the straight step, from the platform's centre
constexpr int kWalkDeadlineS = 420;
constexpr int kStepDeadlineS = 60;

// A leg that gains no ground says INVALID at once, naming where it stalled: the flat distance to its goal must fall by
// `minGainCm` within `spanMs`. The route's own detours can lead away from the goal for a while, so the nav leg's span is long.
constexpr float kNavGainCm = 100.f;
constexpr uint64_t kNavSpanMs = 45000;
constexpr float kStepGainCm = 5.f;
constexpr uint64_t kStepSpanMs = 5000;

float FlatTo(const ue_wrap::FVector& a, const ue_wrap::FVector& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

void ResetProgress(void* me, const ue_wrap::FVector& goal, float* best, uint64_t* at) {
    ue_wrap::FVector pos{};
    E::TryGetActorLocation(me, pos);
    *best = FlatTo(pos, goal);
    *at = ::GetTickCount64();
}

}  // namespace

void Boarder::Begin(void* lift) {
    lift_.Set(lift);
    phase_ = Phase::Wait;
    walk_.reset();
    ring_.clear();
    sides_.clear();
    stand_ = 0;
    Go();
}

bool Boarder::Stalled(void* me, const ue_wrap::FVector& goal, float minGainCm, uint64_t spanMs, const char* what) {
    ue_wrap::FVector pos{};
    if (!E::TryGetActorLocation(me, pos)) return false;
    const float d = FlatTo(pos, goal);
    const uint64_t now = ::GetTickCount64();
    if (d < bestDist_ - minGainCm) {
        bestDist_ = d;
        bestAt_ = now;
        return false;
    }
    if (now - bestAt_ <= spanMs) return false;
    if (walk_) coop::director::CancelBackgroundWalk(*walk_);
    char buf[240];
    std::snprintf(buf, sizeof(buf), "%s stalled: no %.0f cm gained in %llu s; the walker stands at (%.0f,%.0f,%.0f), %.0f cm from "
                  "its goal (%.0f,%.0f), best %.0f cm", what, minGainCm, static_cast<unsigned long long>(spanMs / 1000), pos.X,
                  pos.Y, pos.Z, d, goal.X, goal.Y, bestDist_);
    Invalid(buf);
    return true;
}

// The lift is at rest on this copy: measure its platform and ask which open sides a route reaches.
void Boarder::Prepare(void* me) {
    void* const lift = lift_.Get();
    if (!lift) return Invalid("this copy of the lift was lost");
    bool moving = false;
    if (!CL::ReadMoving(lift, moving)) return Invalid("this copy's lift state did not read");
    if (moving) {   // a copy runs its own travel, from the time it was told: it ends after the host's did
        Late("this copy of the lift never came to rest", kBoundMs);
        return;
    }
    void* const platform = CL::PlatformActor(lift);
    float platZ = 0.f;
    if (!platform || !CL::ReadPlatformZ(lift, platZ) || !ReadPlatform(me, platform, platZ, box_))
        return Invalid("this copy's platform, its bounds or its walkable surface did not read");
    // The way on is the platform's open side: its rails and the room's walls close the others, and an outside ground level with
    // nothing is no floor to stand on. Each open side whose stand point a NavMesh route reaches is a candidate, shortest first.
    ue_wrap::FVector from{};
    if (!E::TryGetActorLocation(me, from)) return Invalid("the rider's place did not read");
    struct Cand {
        std::string name;
        ue_wrap::FVector stand;
        float len;
    };
    std::vector<Cand> cands;
    for (const Side& s : ProbeSides(me, box_, E::GetActorCharacterHalfHeight(me))) {
        float len = 0.f;
        const bool routed = s.open && coop::director::RouteInLegs(me, from, s.stand, kReachCm, &len);
        Fact("side %s: %s -- %s%s", s.name, s.open ? "OPEN" : "closed", s.why.c_str(),
             !s.open ? "" : routed ? " (a route reaches its stand point)" : " (no route reaches its stand point)");
        if (routed) cands.push_back({s.name, s.stand, len});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.len < b.len; });
    for (const Cand& c : cands) {
        ring_.push_back(c.stand);
        sides_.push_back(c.name);
    }
    Fact("platform surface %.1f, floor %.1f; %zu open side(s) with a route", box_.surface, box_.floorZ, ring_.size());
    if (ring_.empty()) {
        phase_ = Phase::Done;
        return;
    }
    stand_ = 0;
    StartLeg(me);
}

// The nav leg to the stand point before the open side stand_.
void Boarder::StartLeg(void* me) {
    walk_ = coop::director::StartBackgroundWalk(ring_[stand_], kReachCm, kWalkDeadlineS);
    if (!walk_) return Invalid("the walk to the platform did not start");
    ResetProgress(me, ring_[stand_], &bestDist_, &bestAt_);
    Fact("open side %s chosen (the floor there is level with the platform's, nothing stands between it and the slab, a route "
         "reaches it): walking to its stand point (%.0f,%.0f) by the director", sides_[stand_].c_str(), ring_[stand_].X,
         ring_[stand_].Y);
    phase_ = Phase::Walk;
    Go();
}

// The step from the stand point onto the platform: a straight leg to the platform's centre through the input seam.
void Boarder::StartStep(void* me) {
    const float standZ = box_.surface + E::GetActorCharacterHalfHeight(me);
    walk_ = coop::director::StartBackgroundWalk({box_.centre.X, box_.centre.Y, standZ}, kStepReachCm, kStepDeadlineS, false,
                                                nullptr, /*straight*/ true);
    if (!walk_) return Invalid("the step onto the platform did not start");
    ResetProgress(me, {box_.centre.X, box_.centre.Y, 0.f}, &bestDist_, &bestAt_);
    Fact("at the %s side's stand point: stepping in across the open edge to the platform's centre by the director's straight leg",
         sides_[stand_].c_str());
    phase_ = Phase::Step;
    Go();
}

Boarder::State Boarder::Tick(void* me) {
    if (Ended()) return State::Failed;
    switch (phase_) {
    case Phase::Wait:
        Prepare(me);
        if (Ended()) return State::Failed;
        return phase_ == Phase::Done ? State::NoRoute : State::Working;
    case Phase::Walk: {
        const int st = walk_ ? walk_->state.load() : 2;
        if (st == 0) {
            if (!Stalled(me, ring_[stand_], kNavGainCm, kNavSpanMs, ("the walk to the " + sides_[stand_] + " side").c_str()))
                Late("the walk to the platform never ended", kWalkBoundMs);
            return Ended() ? State::Failed : State::Working;
        }
        if (st == 2) {   // this stand point was not reached: the next open side
            walk_.reset();
            if (++stand_ >= ring_.size()) {
                Invalid("no open side of the platform could be walked to");
                return State::Failed;
            }
            StartLeg(me);
            return Ended() ? State::Failed : State::Working;
        }
        StartStep(me);
        return Ended() ? State::Failed : State::Working;
    }
    case Phase::Step: {
        const int st = walk_ ? walk_->state.load() : 2;
        if (st == 0) {
            const ue_wrap::FVector centre{box_.centre.X, box_.centre.Y, 0.f};
            if (!Stalled(me, centre, kStepGainCm, kStepSpanMs, ("the step in over the " + sides_[stand_] + " side").c_str()))
                Late("the step onto the platform never ended", kBoundMs);
            return Ended() ? State::Failed : State::Working;
        }
        void* const lift = lift_.Get();
        float platZ = 0.f, past = 0.f, feet = 0.f;
        if (!lift || !CL::ReadPlatformZ(lift, platZ)) {
            Invalid("this copy of the lift was lost");
            return State::Failed;
        }
        if (!StandsOn(me, box_, platZ, &past, &feet) || st == 2) {
            char buf[200];
            std::snprintf(buf, sizeof(buf), "the step ended off the platform (leg %s): %.0f cm past its edge, feet %.0f cm over "
                          "its surface (tolerance %.0f cm, %.0f cm past the edge)", st == 2 ? "failed" : "arrived", past, feet,
                          kFeetTolCm, kEdgeSlackCm);
            Invalid(buf);
            return State::Failed;
        }
        Fact("on the platform: %.0f cm past its edge, feet %.0f cm over its surface", past, feet);
        phase_ = Phase::Done;
        return State::OnPlatform;
    }
    case Phase::Done:
        break;
    }
    return State::Working;
}

}  // namespace coop::dev::basedmove_drill::detail
