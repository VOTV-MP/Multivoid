// coop/dev/basedmove_drill_host.cpp -- the host's steps of the based-movement drill: it names its lift (and the stop it rests at),
// brings the lift to the other stop with the game's own press when the client says no route reaches the platform's floor,
// waits for the client to say it stands on the platform and for the client's puppet to stand there on this copy, presses the
// lift, and logs every frame the numbers the lift's reversal reads and what this engine thinks of the puppet, until the lift
// stops or the Blueprint reverses it. See coop/dev/basedmove_drill.h. The log, chat and verdict are basedmove_drill.cpp.

#include "basedmove_drill_detail.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/character_base.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/cargo_lift.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_component.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

namespace coop::dev::basedmove_drill::detail {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace CL = ue_wrap::cargo_lift;
namespace CB = ue_wrap::character_base;

namespace {

constexpr int kClientSlot = 1;
constexpr uint64_t kStartBoundMs = 20000;   // a press sets `moving` within a tick; this long without it says INVALID

enum class HS { Start, WaitOnboard, Bring, WaitPuppet, Ride, WaitBase };
HS g_h = HS::Start;

ue_wrap::CachedObjRef g_lift;   // the host's lift, held by slot and serial across ticks
PlatformBox g_box;              // the platform's bounds and surface at rest, before the press
int g_stops = 0;                // the rest positions the client was told of: 1, then 2 once the lift was brought to the other
bool g_bringSawMoving = false;
bool g_prevDir = false, g_sawMoving = false;
int g_frames = 0, g_flips = 0;
float g_maxGap = 0.f;
int g_pBase = 0, g_pOther = 0, g_pNone = 0;   // this engine's reading of the puppet's movement base, per frame
std::chrono::steady_clock::time_point g_pressedAt;

void HGo(HS s) {
    g_h = s;
    Go();
}

void* ClientPuppet() {
    coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(kClientSlot);
    return rp && rp->valid() && rp->HasPose() ? rp->GetActor() : nullptr;
}

void* FindLift() {
    for (void* o : R::FindObjectsByClass(L"cargoLift_C"))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) return o;
    return nullptr;
}

// The lift at rest: its platform's bounds and walkable surface, said to the client with the number of this stop. False (said
// INVALID) when anything does not read.
bool RestAndSay(void* lift) {
    void* const platform = CL::PlatformActor(lift);
    if (!platform) return Invalid("the host's lift has no platform component yet (`comp` is set at its begin play)"), false;
    bool moving = false, up = false, ign = false;
    float platZ = 0.f, attZ = 0.f;
    if (!CL::ReadMoving(lift, moving) || !CL::ReadDir(lift, up) || !CL::ReadPlatformZ(lift, platZ) ||
        !CL::ReadAttachZ(lift, attZ) || !CL::ReadIgnoreCheck(lift, ign))
        return Invalid("the lift's state, platform, attach point or ignoreCheck did not read"), false;
    if (moving) return Invalid("the lift is moving where the drill expects it at rest"), false;
    if (!ReadPlatform(coop::players::Registry::Get().Local(), platform, platZ, g_box))
        return Invalid("a down-trace over the platform struck no upward-facing surface of it"), false;
    ++g_stops;
    Fact("lift %s at rest (stop %d): dir=%d platformZ=%.1f attachZ=%.1f ignoreCheck=%d, platform surface %.1f, bounds top %.1f "
         "centre (%.0f,%.0f) half (%.0f,%.0f,%.0f)", NameOf(lift).c_str(), g_stops, up ? 1 : 0, platZ, attZ, ign ? 1 : 0,
         g_box.surface, g_box.top, g_box.centre.X, g_box.centre.Y, g_box.extent.X, g_box.extent.Y, g_box.extent.Z);
    Say("lift " + NameOf(lift) + " " + std::to_string(g_stops));
    return true;
}

void Start() {
    if (!CL::EnsureResolved()) {
        Late("the cargo lift class never resolved", kWalkBoundMs);
        return;
    }
    void* const lift = FindLift();
    if (!lift) return Invalid("the host has no cargo lift");
    g_lift.Set(lift);
    if (RestAndSay(lift)) HGo(HS::WaitOnboard);
}

// The client says no route reaches the platform's floor at this stop: the lift goes to its other stop (the game's own press,
// the call a call button runs), once.
void BringOther(void* lift) {
    if (g_stops >= 2) return Invalid("no route reaches the platform's floor at either stop of the lift");
    if (!CL::Press(lift)) return Invalid("the lift's own press did not run");
    Fact("the client reached no floor beside the platform at stop %d: pressed the lift to bring it to its other stop", g_stops);
    g_bringSawMoving = false;
    HGo(HS::Bring);
}

// The lift's own end of the move is nothing to bind to: `moving` falls after it rose.
void Bring(void* lift) {
    bool moving = false;
    if (!CL::ReadMoving(lift, moving)) return Invalid("the lift's `moving` did not read");
    if (moving) g_bringSawMoving = true;
    if (g_bringSawMoving && !moving) {
        if (RestAndSay(lift)) HGo(HS::WaitOnboard);
        return;
    }
    if (!g_bringSawMoving) Late("the lift never began to move when brought to its other stop", kStartBoundMs);
    else Late("the lift moved to its other stop and did not stop", kRideBoundMs);
}

// The puppet on the platform on this copy: the press waits for it, a few seconds of the stream after the client's step.
void WaitPuppet(void* lift) {
    void* const body = ClientPuppet();
    float platZ = 0.f, past = 0.f, feet = 0.f;
    if (!CL::ReadPlatformZ(lift, platZ)) return Invalid("the platform's height did not read");
    if (!body || !StandsOn(body, g_box, platZ, &past, &feet)) {
        if (coop::net_pump::TickSerial() % 60 == 0)
            Fact("waiting for the puppet on the platform: %s, %.0f cm past its edge, feet %.0f cm over its surface",
                 body ? "placed" : "not placed", past, feet);
        Late("the client's puppet never stood on the platform on the host's copy", kBoundMs);
        return;
    }
    Fact("the puppet stands on the platform: %.0f cm past its edge, feet %.0f cm over its surface", past, feet);
    if (Nocoll()) {
        void* const cap = CB::CapsuleComponent(body);
        const bool ok = E::SetActorEnableCollision(body, false);
        Fact("ride_nocoll: the puppet's collision off (call ran=%d), capsule collision now %d", ok ? 1 : 0,
             cap ? E::GetComponentCollisionEnabled(cap) : -1);
        if (!ok) return Invalid("the puppet's collision could not be switched off");
    }
    bool dir0 = false;
    if (!CL::ReadDir(lift, dir0)) return Invalid("the lift's direction did not read");
    g_box.restZ = platZ;
    if (!CL::Press(lift)) return Invalid("the lift's own press did not run");
    g_pressedAt = std::chrono::steady_clock::now();
    bool dirNow = dir0;
    CL::ReadDir(lift, dirNow);
    g_prevDir = dirNow;
    Fact("pressed: dir %d -> %d", dir0 ? 1 : 0, dirNow ? 1 : 0);
    HGo(HS::Ride);
}

void EndRide(const char* why) {
    if (Nocoll())
        if (void* const body = ClientPuppet()) E::SetActorEnableCollision(body, true);
    Fact("ride ended (%s): flips=%d, largest |attach-comp|=%.1f, frames=%d", why, g_flips, g_maxGap, g_frames);
    Say("end");
    HGo(HS::WaitBase);
}

// One frame of the ride: what the lift's own reversal reads, then the puppet.
void Ride(coop::net::Session& s, void* lift) {
    bool moving = false, dir = false, ign = false;
    float platZ = 0.f, attZ = 0.f;
    if (!CL::ReadMoving(lift, moving) || !CL::ReadDir(lift, dir) || !CL::ReadIgnoreCheck(lift, ign) ||
        !CL::ReadPlatformZ(lift, platZ) || !CL::ReadAttachZ(lift, attZ))
        return Invalid("the lift's state stopped reading during the ride");
    void* const body = ClientPuppet();
    if (!body) return Invalid("the client's puppet was lost during the ride");

    const float gap = attZ - platZ;
    g_maxGap = std::fmax(g_maxGap, std::fabs(gap));
    const bool flipped = dir != g_prevDir;
    if (flipped) ++g_flips;
    g_prevDir = dir;
    if (moving) g_sawMoving = true;

    ue_wrap::FVector at{};
    E::TryGetActorLocation(body, at);
    coop::net::PoseSnapshot pose{};
    const bool havePose = s.TryGetRemotePose(kClientSlot, pose);
    void* const cap = CB::CapsuleComponent(body);
    void* const platform = CL::PlatformActor(lift);
    const bool overlap = cap && platform && E::IsOverlappingActor(cap, platform);
    float past = 0.f, feet = 0.f;
    StandsOn(body, g_box, platZ, &past, &feet);
    CB::Base base;
    CB::ReadMovementBase(body, base);
    if (!base.component) ++g_pNone;
    else if (platform && (base.component == CL::PlatformComponent(lift) || base.owner == platform)) ++g_pBase;
    else ++g_pOther;

    ++g_frames;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_pressedAt).count();
    Fact("f=%d t=%.1fms platZ=%.2f attZ=%.2f attZ-platZ=%.2f dir=%d ignoreCheck=%d moving=%d flip=%d puppetZ=%.2f poseZ=%.2f "
         "feetOverSurface=%.1f capsuleColl=%d overlap=%d puppetBase=%s",
         g_frames, ms, platZ, attZ, gap, dir ? 1 : 0, ign ? 1 : 0, moving ? 1 : 0, flipped ? 1 : 0, at.Z,
         havePose ? pose.z : 0.f, feet, cap ? E::GetComponentCollisionEnabled(cap) : -1, overlap ? 1 : 0,
         BaseName(base.component, base.owner).c_str());

    // The ride ends at the first reversal the Blueprint makes, or when the lift stops after it has moved.
    if (flipped) return EndRide("the lift reversed");
    if (g_sawMoving && !moving) return EndRide("the lift arrived");
    if (!g_sawMoving) Late("the lift never began to move after the press", kStartBoundMs);
    else Late("the lift moved and did not stop", kRideBoundMs);
}

// The client's summary: the base its engine read, over the frames the lift moved and over all of its frames since it boarded.
void WaitBase() {
    std::string rest;
    if (!Rest("base ", &rest)) {
        Late("the client's movement base summary never came");
        return;
    }
    int c[8] = {};
    std::string other;
    std::istringstream in(rest);
    for (int& v : c) in >> v;
    if (!in) return Invalid("the client's movement base summary could not be read");
    in >> other;
    const int cp = c[0], co = c[1], cn = c[2], cf = c[3], ap = c[4], ao = c[5], an = c[6], af = c[7];
    Measured("flips=%d, max |attach-comp|=%.1f, frames=%d; client base while the lift moved (of %d frames): platform=%d "
             "other=%d none=%d, over all %d frames: platform=%d other=%d none=%d, first other base '%s'; host's engine on the "
             "puppet: platform=%d other=%d none=%d",
             g_flips, g_maxGap, g_frames, cf, cp, co, cn, af, ap, ao, an, other.empty() ? "-" : other.c_str(), g_pBase, g_pOther, g_pNone);
    Finish();
}

}  // namespace

void HostTick(coop::net::Session& s) {
    void* const lift = g_lift.Get();
    switch (g_h) {
    case HS::Start:
        if (!HostMayStart(s)) {
            Late("the client's world never became ready", kWalkBoundMs);
            return;
        }
        Start();
        return;
    case HS::WaitOnboard:
        if (!lift) return Invalid("the host's lift was lost");
        if (Seen("onboard")) {
            HGo(HS::WaitPuppet);
            return;
        }
        if (CountOf("noroute ") >= g_stops) {
            BringOther(lift);
            return;
        }
        Late("the client never reached the platform", kWalkBoundMs);
        return;
    case HS::Bring:
        if (!lift) return Invalid("the host's lift was lost");
        Bring(lift);
        return;
    case HS::WaitPuppet:
        if (!lift) return Invalid("the host's lift was lost");
        WaitPuppet(lift);
        return;
    case HS::Ride:
        if (!lift) return Invalid("the host's lift was lost");
        Ride(s, lift);
        return;
    case HS::WaitBase:
        WaitBase();
        return;
    }
}

}  // namespace coop::dev::basedmove_drill::detail
