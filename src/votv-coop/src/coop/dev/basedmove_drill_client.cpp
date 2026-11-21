// coop/dev/basedmove_drill_client.cpp -- the client's steps of the based-movement drill: it finds the host's lift by the name the
// host said, waits for its own copy to rest, finds the platform's open side (sides.cpp) and is walked by the director to the floor in front of it, steps
// in across the open edge onto the platform's surface through the input seam (a short straight leg: the NavMesh does not cover a moving platform), says it
// boarded only when its feet stand on the surface, then logs every frame its own height, its copy of the platform and the
// movement base its engine reads, until the host says the ride ended and is told the tally. A floor no route reaches is told to
// the host, which brings the lift to its other stop; a leg that gains no ground ends the run INVALID at once. See coop/dev/basedmove_drill.h. The log, chat and verdict are
// basedmove_drill.cpp.

#include "basedmove_drill_detail.h"

#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/character_base.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/cargo_lift.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace coop::dev::basedmove_drill::detail {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace CL = ue_wrap::cargo_lift;
namespace CB = ue_wrap::character_base;

namespace {

constexpr float kReachCm = 60.f;       // the director's stop at a standpoint beside the platform
constexpr float kStepReachCm = 30.f;   // the stop of the straight step, from the platform's centre
constexpr int kWalkDeadlineS = 420;
constexpr int kStepDeadlineS = 60;

enum class CS { WaitReady, WaitLift, Walk, Step, Riding };
CS g_c = CS::WaitReady;

ue_wrap::CachedObjRef g_lift;          // this copy of the host's lift
PlatformBox g_box;                     // its platform's bounds and surface when the walk began
int g_lastStop = 0;                    // the host's stop number this client has acted on
std::vector<ue_wrap::FVector> g_ring;  // the stand points before the platform's open sides a route reaches, shortest route first
std::vector<std::string> g_sides;      // their sides' names
size_t g_stand = 0;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
std::chrono::steady_clock::time_point g_boardedAt;

// The engine's reading of this client's base, over the frames the lift moved on this copy and over every frame since it boarded.
struct Tally {
    int platform = 0, other = 0, none = 0, frames = 0;
};
Tally g_moving, g_all;
std::string g_firstOther;
int g_frame = 0;

void CGo(CS s) {
    g_c = s;
    Go();
}

// A leg that gains no ground ends the run at once, naming where it stalled: the flat distance to its goal must fall by
// `minGainCm` within `spanMs`, or the walk is cancelled and the run says INVALID (a time bound says INVALID only). The route's
// own detours can lead away from the goal for a while, so the nav leg's span is long; the straight step's is short.
constexpr float kNavGainCm = 100.f;
constexpr uint64_t kNavSpanMs = 45000;
constexpr float kStepGainCm = 5.f;
constexpr uint64_t kStepSpanMs = 5000;
float g_bestDist = 0.f;
uint64_t g_bestAt = 0;

float FlatTo(const ue_wrap::FVector& a, const ue_wrap::FVector& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

void ResetProgress(void* me, const ue_wrap::FVector& goal) {
    ue_wrap::FVector at{};
    E::TryGetActorLocation(me, at);
    g_bestDist = FlatTo(at, goal);
    g_bestAt = ::GetTickCount64();
}

// True (and INVALID said, the walk cancelled) when the leg to `goal` stalled.
bool Stalled(void* me, const ue_wrap::FVector& goal, float minGainCm, uint64_t spanMs, const char* what) {
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(me, at)) return false;
    const float d = FlatTo(at, goal);
    const uint64_t now = ::GetTickCount64();
    if (d < g_bestDist - minGainCm) {
        g_bestDist = d;
        g_bestAt = now;
        return false;
    }
    if (now - g_bestAt <= spanMs) return false;
    if (g_walk) coop::director::CancelBackgroundWalk(*g_walk);
    char buf[240];
    std::snprintf(buf, sizeof(buf), "%s stalled: no %.0f cm gained in %llu s; the walker stands at (%.0f,%.0f,%.0f), %.0f cm from "
                  "its goal (%.0f,%.0f), best %.0f cm", what, minGainCm, static_cast<unsigned long long>(spanMs / 1000), at.X,
                  at.Y, at.Z, d, goal.X, goal.Y, g_bestDist);
    Invalid(buf);
    return true;
}

// The nav leg to the stand point before the open side g_stand.
void StartLeg(void* me) {
    g_walk = coop::director::StartBackgroundWalk(g_ring[g_stand], kReachCm, kWalkDeadlineS);
    if (!g_walk) return Invalid("the walk to the platform did not start");
    ResetProgress(me, g_ring[g_stand]);
    Fact("open side %s chosen (the floor there is level with the platform's, nothing stands between it and the slab, a route "
         "reaches it): walking to its stand point (%.0f,%.0f) by the director", g_sides[g_stand].c_str(), g_ring[g_stand].X,
         g_ring[g_stand].Y);
    CGo(CS::Walk);
}

void* FindLiftNamed(const std::string& name) {
    for (void* o : R::FindObjectsByClass(L"cargoLift_C"))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__") && NameOf(o) == name) return o;
    return nullptr;
}

// The host's line `lift <name> <stop>`; true with the name and stop when it is a stop this client has not acted on.
bool NewStop(std::string* name, int* stop) {
    std::string text;
    if (!Rest("lift ", &text)) return false;
    std::istringstream in(text);
    std::string n;
    int k = 0;
    if (!(in >> n >> k) || k <= g_lastStop) return false;
    *name = n;
    *stop = k;
    return true;
}

// The lift is at rest on this copy: measure its platform and ask the engine for the points beside it that a route reaches.
void Approach(void* me, const std::string& name, int stop) {
    if (!CL::EnsureResolved()) {
        Late("the cargo lift class never resolved", kWalkBoundMs);
        return;
    }
    void* const lift = FindLiftNamed(name);
    if (!lift) return Invalid("the client has no cargo lift of the host's name");
    bool moving = false;
    if (!CL::ReadMoving(lift, moving)) return Invalid("this copy's lift state did not read");
    if (moving) {   // its own travel, run from the time it was told, ends after the host's did
        Late("this copy of the lift never came to rest", kBoundMs);
        return;
    }
    g_lift.Set(lift);
    void* const platform = CL::PlatformActor(lift);
    float platZ = 0.f;
    if (!platform || !CL::ReadPlatformZ(lift, platZ) || !ReadPlatform(me, platform, platZ, g_box))
        return Invalid("this copy's platform, its bounds or its walkable surface did not read");
    g_lastStop = stop;
    // The way on is the platform's open side: its rails and the room's walls close the others, and an outside ground level with
    // nothing is no floor to stand on. Each open side whose stand point a NavMesh route reaches is a candidate, shortest first.
    ue_wrap::FVector from{};
    if (!E::TryGetActorLocation(me, from)) return Invalid("the client's place did not read");
    struct Cand {
        std::string name;
        ue_wrap::FVector stand;
        float len;
    };
    std::vector<Cand> cands;
    for (const Side& s : ProbeSides(me, g_box, E::GetActorCharacterHalfHeight(me))) {
        float len = 0.f;
        const bool routed = s.open && coop::director::RouteInLegs(me, from, s.stand, kReachCm, &len);
        Fact("side %s: %s -- %s%s", s.name, s.open ? "OPEN" : "closed", s.why.c_str(),
             !s.open ? "" : routed ? " (a route reaches its stand point)" : " (no route reaches its stand point)");
        if (routed) cands.push_back({s.name, s.stand, len});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.len < b.len; });
    g_ring.clear();
    g_sides.clear();
    for (const Cand& c : cands) {
        g_ring.push_back(c.stand);
        g_sides.push_back(c.name);
    }
    Fact("stop %d: platform surface %.1f, floor %.1f; %zu open side(s) with a route", stop, g_box.surface, g_box.floorZ,
         g_ring.size());
    if (g_ring.empty()) {   // this floor has no way to the platform: the host brings the lift to its other stop
        Say("noroute " + std::to_string(stop));
        CGo(CS::WaitLift);
        return;
    }
    g_stand = 0;
    StartLeg(me);
}

void CountBase(void* me, void* lift, bool moving, std::string* baseText) {
    CB::Base base;
    CB::ReadMovementBase(me, base);
    void* const platform = CL::PlatformActor(lift);
    const bool onPlatform = platform && base.component && (base.component == CL::PlatformComponent(lift) || base.owner == platform);
    for (Tally* t : {&g_all, &g_moving}) {
        if (t == &g_moving && !moving) continue;
        ++t->frames;
        if (!base.component) ++t->none;
        else if (onPlatform) ++t->platform;
        else ++t->other;
    }
    if (base.component && !onPlatform && g_firstOther.empty()) g_firstOther = BaseName(base.component, base.owner);
    *baseText = onPlatform ? "platform " + BaseName(base.component, base.owner) : BaseName(base.component, base.owner);
}

// One frame of the ride on this copy.
void RideFrame(void* me, void* lift) {
    bool moving = false, dir = false;
    float platZ = 0.f, attZ = 0.f;
    CL::ReadMoving(lift, moving);
    CL::ReadDir(lift, dir);
    CL::ReadPlatformZ(lift, platZ);
    CL::ReadAttachZ(lift, attZ);
    ue_wrap::FVector at{};
    E::TryGetActorLocation(me, at);
    std::string base;
    CountBase(me, lift, moving, &base);
    ++g_frame;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_boardedAt).count();
    float past = 0.f, feet = 0.f;
    StandsOn(me, g_box, platZ, &past, &feet);
    Fact("f=%d t=%.1fms ownZ=%.2f platZ=%.2f attZ=%.2f dir=%d moving=%d feetOverSurface=%.1f base=%s", g_frame, ms, at.Z, platZ,
         attZ, dir ? 1 : 0, moving ? 1 : 0, feet, base.c_str());
}

void SendTally() {
    char line[200];
    std::snprintf(line, sizeof(line), "base %d %d %d %d %d %d %d %d %s", g_moving.platform, g_moving.other, g_moving.none,
                  g_moving.frames, g_all.platform, g_all.other, g_all.none, g_all.frames,
                  g_firstOther.empty() ? "-" : g_firstOther.c_str());
    Fact("movement base: while the lift moved on this copy (%d frames) platform=%d other=%d none=%d; over all %d frames "
         "platform=%d other=%d none=%d; first other '%s'",
         g_moving.frames, g_moving.platform, g_moving.other, g_moving.none, g_all.frames, g_all.platform, g_all.other,
         g_all.none, g_firstOther.empty() ? "-" : g_firstOther.c_str());
    Say(line);
}

// The step from the standpoint onto the platform: a straight leg to the platform's centre through the input seam.
void StartStep(void* me) {
    const float standZ = g_box.surface + E::GetActorCharacterHalfHeight(me);
    g_walk = coop::director::StartBackgroundWalk({g_box.centre.X, g_box.centre.Y, standZ}, kStepReachCm, kStepDeadlineS, false,
                                                 nullptr, /*straight*/ true);
    if (!g_walk) return Invalid("the step onto the platform did not start");
    ResetProgress(me, {g_box.centre.X, g_box.centre.Y, 0.f});
    Fact("at the %s side's stand point: stepping in across the open edge to the platform's centre by the director's straight leg",
         g_sides[g_stand].c_str());
    CGo(CS::Step);
}

}  // namespace

void ClientTick(coop::net::Session&) {
    void* const me = coop::players::Registry::Get().Local();
    if (!me || !R::IsLive(me)) return;
    switch (g_c) {
    case CS::WaitReady:
        if (!ClientMayStart()) {
            Late("the client's world never became ready", kWalkBoundMs);
            return;
        }
        Fact("in the host's world");
        CGo(CS::WaitLift);
        return;
    case CS::WaitLift: {
        std::string name;
        int stop = 0;
        if (!NewStop(&name, &stop)) {
            Late("the host never named its lift at a stop", kWalkBoundMs);
            return;
        }
        Approach(me, name, stop);
        return;
    }
    case CS::Walk: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) {
            if (!Stalled(me, g_ring[g_stand], kNavGainCm, kNavSpanMs, ("the walk to the " + g_sides[g_stand] + " side").c_str()))
                Late("the walk to the platform never ended", kWalkBoundMs);
            return;
        }
        if (st == 2) {   // this stand point was not reached: the next open side
            g_walk.reset();
            if (++g_stand >= g_ring.size()) return Invalid("no open side of the platform could be walked to");
            StartLeg(me);
            return;
        }
        StartStep(me);
        return;
    }
    case CS::Step: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) {
            const ue_wrap::FVector centre{g_box.centre.X, g_box.centre.Y, 0.f};
            if (!Stalled(me, centre, kStepGainCm, kStepSpanMs, ("the step in over the " + g_sides[g_stand] + " side").c_str()))
                Late("the step onto the platform never ended", kBoundMs);
            return;
        }
        void* const lift = g_lift.Get();
        float platZ = 0.f, past = 0.f, feet = 0.f;
        if (!lift || !CL::ReadPlatformZ(lift, platZ)) return Invalid("this copy of the lift was lost");
        if (!StandsOn(me, g_box, platZ, &past, &feet) || st == 2) {
            char buf[200];
            std::snprintf(buf, sizeof(buf), "the step ended off the platform (leg %s): %.0f cm past its edge, feet %.0f cm over "
                          "its surface (tolerance %.0f cm, %.0f cm past the edge)", st == 2 ? "failed" : "arrived", past, feet,
                          kFeetTolCm, kEdgeSlackCm);
            return Invalid(buf);
        }
        Fact("on the platform: %.0f cm past its edge, feet %.0f cm over its surface", past, feet);
        g_boardedAt = std::chrono::steady_clock::now();
        Say("onboard");
        CGo(CS::Riding);
        return;
    }
    case CS::Riding: {
        void* const lift = g_lift.Get();
        if (!lift) return Invalid("this copy of the lift was lost");
        if (Seen("end")) {
            SendTally();
            Finish();
            return;
        }
        RideFrame(me, lift);
        Late("the host never said the ride ended", kWalkBoundMs);
        return;
    }
    }
}

}  // namespace coop::dev::basedmove_drill::detail
