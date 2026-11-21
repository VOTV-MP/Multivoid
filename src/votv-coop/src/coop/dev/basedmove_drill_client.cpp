// coop/dev/basedmove_drill_client.cpp -- the client's steps of the based-movement drill. In ride and the bad_base arms it finds the
// host's lift by the name the host said and boards it (basedmove_drill_rider.cpp); it says it boarded, logs every frame its own
// height, its copy of the platform and the movement base its engine reads, and in a bad_base arm forges its claim when the host
// says so, until the host ends the run. In hostride it watches the host's puppet against its own copy of the platform and tells
// the host what it read. See coop/dev/basedmove_drill.h. The log, chat and verdict are basedmove_drill.cpp.

#include "basedmove_drill_detail.h"

#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/pose_bases.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/actors/character_base.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/cargo_lift.h"
#include "ue_wrap/engine/engine.h"

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
namespace PB = coop::pose_bases;

namespace {

constexpr float kForgedXyCm = 200.f;     // bad_base_xy: the claimed offset lies this far from the absolute pose (the check's cap is 30)
constexpr float kForgedLiftCm = 3000.f;  // bad_base_vol: the absolute height lies this far above the pose's true one (the travel is 500)

enum class CS { WaitReady, WaitLift, Board, Riding, WaitHostOnboard, WaitPuppet, Observing };
CS g_c = CS::WaitReady;

Boarder g_boarder;
Watcher g_watch;
PlatformBox g_box;                     // hostride: the platform copy's bounds and surface, read when the host boarded
std::string g_name;                    // the host's lift, by name
ue_wrap::CachedObjRef g_lift;          // this copy of it
int g_lastStop = 0;                    // the host's stop number this client has acted on
bool g_forging = false;
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

// A hostile client's claim, as the sender fills it (pose_bases::SetOutboundForge): the offset moved, or the absolute height.
void ForgeXy(coop::net::PoseSnapshot& pose) { pose.bx += kForgedXyCm; }
void ForgeVol(coop::net::PoseSnapshot& pose) { pose.z += kForgedLiftCm; }

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
    Fact("f=%d t=%.1fms ownZ=%.2f platZ=%.2f attZ=%.2f dir=%d moving=%d forging=%d base=%s", g_frame, ms, at.Z, platZ, attZ,
         dir ? 1 : 0, moving ? 1 : 0, g_forging ? 1 : 0, base.c_str());
}

void LogTally() {
    Fact("movement base: while the lift moved on this copy (%d frames) platform=%d other=%d none=%d; over all %d frames "
         "platform=%d other=%d none=%d; first other '%s'",
         g_moving.frames, g_moving.platform, g_moving.other, g_moving.none, g_all.frames, g_all.platform, g_all.other,
         g_all.none, g_firstOther.empty() ? "-" : g_firstOther.c_str());
}

// ---- the arms where the client rides ------------------------------------------------------------------------------------------
void BeginBoarding(const std::string& name, int stop) {
    void* const lift = FindLiftNamed(name);
    if (!lift) return Invalid("the client has no cargo lift of the host's name");
    if (!CL::EnsureResolved()) {
        Late("the cargo lift class never resolved", kWalkBoundMs);
        return;
    }
    g_lastStop = stop;
    g_lift.Set(lift);
    g_boarder.Begin(lift);
    CGo(CS::Board);
}

void Board(void* me) {
    switch (g_boarder.Tick(me)) {
    case Boarder::State::OnPlatform:
        g_boardedAt = std::chrono::steady_clock::now();
        Say("onboard");
        CGo(CS::Riding);
        return;
    case Boarder::State::NoRoute:   // this floor has no way to the platform: the host brings the lift to its other stop
        Say("noroute " + std::to_string(g_lastStop));
        CGo(CS::WaitLift);
        return;
    default:
        return;
    }
}

void Riding(void* me) {
    void* const lift = g_lift.Get();
    if (!lift) return Invalid("this copy of the lift was lost");
    if (Seen("end")) {
        PB::SetOutboundForge(nullptr);
        LogTally();
        Finish();
        return;
    }
    if (Hostile() && !g_forging && Seen("forge")) {   // the host sees the client on its platform: the claim is forged from now on
        g_forging = true;
        PB::SetOutboundForge(ArmOf() == Arm::BadXy ? &ForgeXy : &ForgeVol);
        Fact("forging the claim from now on (%s)", ArmName());
    }
    RideFrame(me, lift);
    Late("the host never said the run ended", kWalkBoundMs);
}

// ---- hostride: the client watches the host's puppet ---------------------------------------------------------------------------
// The host has boarded and its lift is at rest here: the platform copy's bounds and surface, then the host's puppet on it.
void WaitHostOnboard(void* me) {
    if (!Seen("hostonboard")) {
        Late("the host never boarded", kWalkBoundMs);
        return;
    }
    void* const lift = FindLiftNamed(g_name);
    if (!lift) return Invalid("the client has no cargo lift of the host's name");
    bool moving = false;
    if (!CL::ReadMoving(lift, moving)) return Invalid("this copy's lift state did not read");
    if (moving) {   // its own travel, run from the time it was told, ends after the host's did
        Late("this copy of the lift never came to rest", kBoundMs);
        return;
    }
    void* const platform = CL::PlatformActor(lift);
    float platZ = 0.f;
    if (!platform || !CL::ReadPlatformZ(lift, platZ) || !ReadPlatform(me, platform, platZ, g_box))
        return Invalid("this copy's platform, its bounds or its walkable surface did not read");
    g_lift.Set(lift);
    CGo(CS::WaitPuppet);
}

void WaitPuppet(void* lift) {
    coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(0);
    void* const body = rp && rp->valid() && rp->HasPose() ? rp->GetActor() : nullptr;
    float platZ = 0.f, past = 0.f, feet = 0.f;
    if (!CL::ReadPlatformZ(lift, platZ)) return Invalid("the platform's height did not read");
    if (!body || !StandsOn(body, g_box, platZ, &past, &feet)) {
        Late("the host's puppet never stood on this copy of the platform", kBoundMs);
        return;
    }
    Fact("the host's puppet stands on the platform: %.0f cm past its edge, feet %.0f cm over its surface", past, feet);
    g_watch.Begin(0);
    Say("seen");
    CGo(CS::Observing);
}

void Observing(coop::net::Session& s, void* lift) {
    if (Seen("end")) {
        char line[200];
        std::snprintf(line, sizeof(line), "result %d %d %.2f %.2f %d %d %.2f", g_watch.frames, g_watch.flips, g_watch.maxD,
                      g_watch.maxFeet, g_watch.basedFrames, g_watch.attachedFrames, g_watch.maxGap);
        Say(line);
        Finish();
        return;
    }
    const Watched w = g_watch.Frame(s, lift, g_box);
    if (!w.ok) return;   // the puppet or the lift did not read this frame: the next
    // The verdict is known at the first frame the host's puppet is off the slab on this copy: said now, with nothing after it.
    if (std::fabs(w.feet) > kFeetTolCm) {
        Fail("the host's puppet left the slab on this copy at frame %d: feet %.1f cm over its surface (limit %.0f cm), d=%.1f, "
             "with a base %d of %d frames, attached %d", g_watch.frames, w.feet, kFeetTolCm, w.d, g_watch.basedFrames,
             g_watch.frames, g_watch.attachedFrames);
        return;
    }
    Late("the host never said its ride ended", kWalkBoundMs);
}

}  // namespace

void ClientTick(coop::net::Session& s) {
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
        g_name = name;
        if (ArmOf() == Arm::HostRide) {   // the host boards, whatever stop it settles on: the platform is read when it has
            g_lastStop = stop;
            CGo(CS::WaitHostOnboard);
            return;
        }
        BeginBoarding(name, stop);
        return;
    }
    case CS::Board:
        Board(me);
        return;
    case CS::Riding:
        Riding(me);
        return;
    case CS::WaitHostOnboard:
        WaitHostOnboard(me);
        return;
    case CS::WaitPuppet: {
        void* const lift = g_lift.Get();
        if (!lift) return Invalid("this copy of the lift was lost");
        WaitPuppet(lift);
        return;
    }
    case CS::Observing: {
        void* const lift = g_lift.Get();
        if (!lift) return Invalid("this copy of the lift was lost");
        Observing(s, lift);
        return;
    }
    }
}

}  // namespace coop::dev::basedmove_drill::detail
