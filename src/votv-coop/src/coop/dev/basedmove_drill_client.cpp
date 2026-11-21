// coop/dev/basedmove_drill_client.cpp -- the client's steps of the based-movement drill: it finds the host's lift by the name the
// host said, is walked by the director onto a point of the platform that a route reaches, says it stands there, then logs every
// frame its own height, its copy of the platform and the movement base its engine reads, until the host says the ride ended and
// is told the tally. See coop/dev/basedmove_drill.h. The log, chat and verdict are basedmove_drill.cpp.

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

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::basedmove_drill::detail {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace CL = ue_wrap::cargo_lift;
namespace CB = ue_wrap::character_base;

namespace {

constexpr float kReachCm = 60.f;       // the director's stop at a standpoint on the platform
constexpr int kWalkDeadlineS = 420;
constexpr int kStandpoints = 3;

enum class CS { WaitReady, WaitLift, Walk, Riding };
CS g_c = CS::WaitReady;

ue_wrap::CachedObjRef g_lift;          // this copy of the host's lift
PlatformBox g_box;                     // its platform's bounds when the walk began
std::vector<ue_wrap::FVector> g_ring;  // the points over the platform a route reaches, shortest route first
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

void* FindLiftNamed(const std::string& name) {
    for (void* o : R::FindObjectsByClass(L"cargoLift_C"))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__") && NameOf(o) == name) return o;
    return nullptr;
}

void StartWalk(void* me, const std::string& name) {
    if (!CL::EnsureResolved()) {
        Late("the cargo lift class never resolved", kWalkBoundMs);
        return;
    }
    void* const lift = FindLiftNamed(name);
    if (!lift) return Invalid("the client has no cargo lift of the host's name");
    g_lift.Set(lift);
    void* const platform = CL::PlatformActor(lift);
    if (!platform || !ReadPlatformBox(platform, g_box)) return Invalid("this copy's platform or its bounds did not read");
    const float halfHeight = E::GetActorCharacterHalfHeight(me);
    const float standZ = g_box.top + halfHeight;
    const float ring = (std::max)(30.f, 0.5f * (std::min)(g_box.extent.X, g_box.extent.Y));
    std::string trace;
    g_ring = coop::director::ReachableStandpoints(me, {g_box.centre.X, g_box.centre.Y, standZ}, ring, kStandpoints, kReachCm,
                                                  &standZ, &trace);
    Fact("a route reaches %zu of %d standpoints over the platform (top %.1f, ring %.0f cm)%s%s", g_ring.size(), kStandpoints,
         g_box.top, ring, trace.empty() ? "" : ": ", trace.c_str());
    if (g_ring.empty()) return Invalid("no route reaches a point over the platform");
    g_stand = 0;
    g_walk = coop::director::StartBackgroundWalk(g_ring[g_stand], kReachCm, kWalkDeadlineS);
    if (!g_walk) return Invalid("the walk onto the platform did not start");
    Fact("walking onto the platform by the director");
    CGo(CS::Walk);
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
    StandsOn(me, g_box, &past, &feet);
    Fact("f=%d t=%.1fms ownZ=%.2f platZ=%.2f attZ=%.2f dir=%d moving=%d feetOverTop=%.1f base=%s", g_frame, ms, at.Z, platZ, attZ,
         dir ? 1 : 0, moving ? 1 : 0, feet, base.c_str());
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
        if (!Rest("lift ", &name)) {
            Late("the host never named its lift", kWalkBoundMs);
            return;
        }
        StartWalk(me, name);
        return;
    }
    case CS::Walk: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) {
            Late("the walk onto the platform never ended", kWalkBoundMs);
            return;
        }
        if (st == 2) {   // this standpoint was not reached: the next
            g_walk.reset();
            if (++g_stand >= g_ring.size()) return Invalid("no standpoint over the platform could be walked to");
            g_walk = coop::director::StartBackgroundWalk(g_ring[g_stand], kReachCm, kWalkDeadlineS);
            if (!g_walk) return Invalid("the walk onto the platform did not start");
            Go();
            return;
        }
        float past = 0.f, feet = 0.f;
        if (!StandsOn(me, g_box, &past, &feet)) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "the walk ended off the platform: %.0f cm past its edge, feet %.0f cm over its top",
                          past, feet);
            return Invalid(buf);
        }
        Fact("on the platform: %.0f cm past its edge, feet %.0f cm over its top", past, feet);
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
