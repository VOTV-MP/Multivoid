// coop/dev/basedmove_drill_host.cpp -- the host's steps of the based-movement drill: it names its lift (and the stop it rests at),
// brings the lift to its other stop with the game's own press when the rider reaches no floor beside the platform, and then,
// by arm: ride -- waits for the client's puppet to stand on the platform, presses the lift and judges every frame (PASS at the
// lift's stop, FAIL at a reversal); hostride -- boards itself, presses, and prints the client's measure; bad_base_* -- waits
// for the client's forged claim and judges what it does with it. See coop/dev/basedmove_drill.h. The log, chat and verdict
// are basedmove_drill.cpp, the boarding basedmove_drill_rider.cpp, the per-frame reads basedmove_drill_watch.cpp.

#include "basedmove_drill_detail.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/pose_bases.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/actor_end_play.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/devices/cargo_lift.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

namespace coop::dev::basedmove_drill::detail {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace CL = ue_wrap::cargo_lift;
namespace PB = coop::pose_bases;

namespace {

constexpr int kClientSlot = 1;
constexpr uint64_t kStartBoundMs = 20000;   // a press sets `moving` within a tick; this long without it says INVALID
constexpr uint32_t kRejectsToPass = 5;      // forged claims set aside before the host's puppet is judged
constexpr uint32_t kAcceptsToFail = 10;     // forged claims let through: the check is not holding

enum class HS { Start, WaitOnboard, Bring, WaitPuppet, Ride, Hostile, Board, WaitSeen, HostRide, WaitResult, EndJudge };
HS g_h = HS::Start;

ue_wrap::CachedObjRef g_lift;   // the host's lift, held by slot and serial across ticks
PlatformBox g_box;              // the platform's bounds and surface at rest, before the press (the ride arms)
Boarder g_boarder;              // hostride: the host boards itself
Watcher g_watch;                // ride: the client's puppet against this lift
int g_stops = 0;                // the rest positions the rider was told of: 1, then 2 once the lift was brought to the other
bool g_bringSawMoving = false;
bool g_sawMoving = false, g_prevDir = false;   // hostride: the host's own lift
bool g_forgedSeen = false;
uint32_t g_accepted0 = 0;
ue_wrap::FVector g_endPre{};     // baseend: where the puppet stood the frame before its platform was destroyed
void* g_endActor = nullptr;      // the platform actor the drill destroyed, compared by pointer inside the engine's end of play
bool g_endSeen = false;          // its end of play came

void HGo(HS s) {
    g_h = s;
    Go();
}

void* PuppetOf(int slot) {
    coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    return rp && rp->valid() && rp->HasPose() ? rp->GetActor() : nullptr;
}

void* FindLift() {
    for (void* o : R::FindObjectsByClass(L"cargoLift_C"))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) return o;
    return nullptr;
}

// The lift at rest: its platform's bounds and walkable surface, said to the rider with the number of this stop. False (said
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

// The lift is at rest at a stop the rider is to board from: the next step by arm.
void Rested(void* lift) {
    if (ArmOf() == Arm::HostRide) {
        g_boarder.Begin(lift);
        HGo(HS::Board);
    } else {
        HGo(HS::WaitOnboard);
    }
}

void EndSink(void* actor, ue_wrap::actor_end_play::Reason);

void Start() {
    if (!CL::EnsureResolved()) {
        Late("the cargo lift class never resolved", kWalkBoundMs);
        return;
    }
    void* const lift = FindLift();
    if (!lift) return Invalid("the host has no cargo lift");
    g_lift.Set(lift);
    if (ArmOf() == Arm::BaseEnd && !ue_wrap::actor_end_play::AddSink(&EndSink)) return Invalid("the end-of-play sink table is full");
    if (RestAndSay(lift)) Rested(lift);
}

// The rider reaches no floor beside the platform at this stop: the lift goes to its other stop (the game's own press, the
// call a call button runs), once.
void BringOther(void* lift) {
    if (g_stops >= 2) return Invalid("no route reaches the platform's floor at either stop of the lift");
    if (!CL::Press(lift)) return Invalid("the lift's own press did not run");
    Fact("the rider reached no floor beside the platform at stop %d: pressed the lift to bring it to its other stop", g_stops);
    g_bringSawMoving = false;
    HGo(HS::Bring);
}

// The lift's own end of the move is nothing to bind to: `moving` falls after it rose.
void Bring(void* lift) {
    bool moving = false;
    if (!CL::ReadMoving(lift, moving)) return Invalid("the lift's `moving` did not read");
    if (moving) g_bringSawMoving = true;
    if (g_bringSawMoving && !moving) {
        if (RestAndSay(lift)) Rested(lift);
        return;
    }
    if (!g_bringSawMoving) Late("the lift never began to move when brought to its other stop", kStartBoundMs);
    else Late("the lift moved to its other stop and did not stop", kRideBoundMs);
}

// ---- ride and the hostile arms: the client rides ------------------------------------------------------------------------------
// The puppet on the platform on this copy: the next step waits for it, a few seconds of the stream after the rider's step.
void WaitPuppet(void* lift) {
    void* const body = PuppetOf(kClientSlot);
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
    if (Hostile()) {   // the rider forges its claim now; the host judges what the receiver does with it
        Say("forge");
        g_forgedSeen = false;
        HGo(HS::Hostile);
        return;
    }
    g_box.restZ = platZ;
    if (!CL::Press(lift)) return Invalid("the lift's own press did not run");
    g_watch.Begin(kClientSlot);
    Fact("pressed");
    HGo(HS::Ride);
}

void DestroyWhenAttached(const Watched& w, void* lift);

// One frame of the ride: PASS when the lift reaches its stop, FAIL at the first reversal.
void Ride(coop::net::Session& s, void* lift) {
    const Watched w = g_watch.Frame(s, lift, g_box);
    if (!w.ok) return Invalid("the lift or the client's puppet stopped reading during the ride");
    if (ArmOf() == Arm::BaseEnd) return DestroyWhenAttached(w, lift);
    if (w.flipped) {
        Say("end");
        return Fail("the lift reversed at frame %d (its own block test read a gap of %.1f cm against 10): max |attach-comp|=%.1f, "
                    "max d=%.1f, max |feet|=%.1f, frames with a base %d of %d, attached %d", g_watch.frames, w.gap, g_watch.maxGap,
                    g_watch.maxD, g_watch.maxFeet, g_watch.basedFrames, g_watch.frames, g_watch.attachedFrames);
    }
    if (g_watch.SawMoving() && !w.moving) {
        Say("end");
        if (g_watch.basedFrames == 0)
            return Invalid("the lift reached its stop, but the client's pose never named a base: nothing was judged");
        return Pass("the lift reached its stop with no reversal: frames=%d, max |attach-comp|=%.1f, max d=%.1f, max |feet|=%.1f, "
                    "frames with a base %d, attached %d", g_watch.frames, g_watch.maxGap, g_watch.maxD, g_watch.maxFeet,
                    g_watch.basedFrames, g_watch.attachedFrames);
    }
    if (!g_watch.SawMoving()) Late("the lift never began to move after the press", kStartBoundMs);
    else Late("the lift moved and did not stop", kRideBoundMs);
}

// A forged pose, as this receiver reads it: the XY gap past the check's cap (bad_base_xy) or an absolute height past the lift's
// travel (bad_base_vol).
bool Forged(const coop::net::PoseSnapshot& p, void* lift) {
    if (p.baseKind != coop::net::pose_base_kind::kMover) return false;
    PB::Resolved at;
    if (!PB::Resolve(p.baseId, at)) return false;
    if (ArmOf() == Arm::BadXy) {
        const float gx = p.x - (at.origin.X + p.bx), gy = p.y - (at.origin.Y + p.by);
        return std::sqrt(gx * gx + gy * gy) > PB::kMaxXyGapCm;
    }
    float low = 0.f, high = 0.f;
    return CL::ReadStopsZ(lift, low, high) && p.z > high + PB::kAboveStopCm;
}

// The hostile arms: once the forged pose is in, the claim must be set aside (counted) and the puppet must follow the absolute
// pose; a puppet that rides the forged claim, its claims let through, is the failure. Decided by the counts of poses.
void JudgeForged(coop::net::Session& s, void* lift) {
    coop::net::PoseSnapshot pose{};
    if (!s.TryGetRemotePose(kClientSlot, pose)) return;
    if (!g_forgedSeen) {
        if (!Forged(pose, lift)) {
            Late("the client's forged pose never arrived", kBoundMs);
            return;
        }
        g_forgedSeen = true;
        g_accepted0 = PB::Accepted(kClientSlot);
        Fact("the forged pose arrived: abs (%.0f,%.0f,%.0f), offset (%.0f,%.0f,%.0f)", pose.x, pose.y, pose.z, pose.bx, pose.by,
             pose.bz);
        Go();
    }
    const PB::Reject why = ArmOf() == Arm::BadXy ? PB::Reject::XyGap : PB::Reject::Volume;
    const uint32_t rejected = PB::Rejected(kClientSlot, why), accepted = PB::Accepted(kClientSlot) - g_accepted0;
    void* const body = PuppetOf(kClientSlot);
    void* const platform = CL::PlatformActor(lift);
    void* parent = nullptr;
    const bool attached = body && platform && E::GetAttachParentActor(body, parent) && parent == platform;
    ue_wrap::FVector at{};
    if (!body || !E::TryGetActorLocation(body, at)) return Invalid("the client's puppet was lost");
    const float off = std::sqrt((at.X - pose.x) * (at.X - pose.x) + (at.Y - pose.y) * (at.Y - pose.y) +
                                (at.Z - pose.z) * (at.Z - pose.z));
    if (accepted >= kAcceptsToFail) {
        Say("end");
        return Fail("%u forged claims were let through (%u set aside): the puppet rides the forged offset, %.0f cm from the "
                    "absolute pose, attached=%d", accepted, rejected, off, attached ? 1 : 0);
    }
    if (rejected >= kRejectsToPass && !attached && off <= PB::kMaxXyGapCm) {
        Say("end");
        return Pass("%u forged claims were set aside and none let through; the puppet follows the absolute pose (%.0f cm off), "
                    "attached=%d", rejected, off, attached ? 1 : 0);
    }
    Late("the host neither set the forged claims aside nor let them through", kBoundMs);
}

// ---- baseend: the host destroys the platform under the rider -------------------------------------------------------------------
// The engine's end of the destroyed platform's play, seen by the drill's own sink: the event the judge waits for. There is no red
// for this arm: it proves what the ENGINE does with an attached puppet when its parent is destroyed (batch4: detached, in place).
void EndSink(void* actor, ue_wrap::actor_end_play::Reason) {
    if (actor && actor == g_endActor) g_endSeen = true;
}

// Mid-travel, once the puppet is attached to the platform (the event): the drill destroys its own copy of the platform actor
// with the game's K2_DestroyActor and judges the puppet at the next frame.
void DestroyWhenAttached(const Watched& w, void* lift) {
    if (w.flipped) return Invalid("the lift reversed before the drill destroyed its platform");
    if (g_watch.SawMoving() && w.moving && w.attached) {
        void* const platform = CL::PlatformActor(lift);
        if (!platform) return Invalid("the lift's platform actor was not there to destroy");
        g_endPre = w.at;
        g_endActor = platform;
        g_endSeen = false;
        Fact("the drill destroys the host's copy of the platform actor %s (K2_DestroyActor) mid-travel, frame %d; the puppet stands at "
             "(%.1f,%.1f,%.1f), attached", NameOf(platform).c_str(), g_watch.frames, w.at.X, w.at.Y, w.at.Z);
        if (!E::DestroyActor(platform)) return Invalid("K2_DestroyActor did not run on the platform actor");
        HGo(HS::EndJudge);
        return;
    }
    if (g_watch.SawMoving() && !w.moving) return Invalid("the lift reached its stop and the puppet never attached to the platform");
    if (!g_watch.SawMoving()) Late("the lift never began to move after the press", kStartBoundMs);
    else Late("the lift moved and did not stop", kRideBoundMs);
}

// The frame after the platform's end of play: the puppet still exists, hangs from no parent and stands where it stood.
void JudgeEnd() {
    if (!g_endSeen) {
        Late("the platform's end of play never came after its destroy", kStartBoundMs);
        return;
    }
    Say("end");
    coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(kClientSlot);
    void* const body = rp && rp->valid() ? rp->GetActor() : nullptr;
    if (!body) return Fail("the puppet was destroyed with its base");
    ue_wrap::FVector at{}, rootLoc{};
    ue_wrap::FRotator rootRot{};
    if (!E::TryGetActorLocation(body, at)) return Invalid("the puppet's place did not read after its base ended");
    if (!E::TryGetRootPoseRaw(body, rootLoc, rootRot))
        return Fail("the puppet is still parented to the ended platform (its root's attach parent is set)");
    const float dx = at.X - g_endPre.X, dy = at.Y - g_endPre.Y, dz = at.Z - g_endPre.Z;
    const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (moved > PB::kMaxXyGapCm)
        return Fail("the puppet moved %.0f cm when its base ended (it stood at (%.1f,%.1f,%.1f), now (%.1f,%.1f,%.1f))", moved,
                    g_endPre.X, g_endPre.Y, g_endPre.Z, at.X, at.Y, at.Z);
    Pass("the platform's end of play came; the puppet still exists, has no parent and moved %.1f cm", moved);
}

// ---- hostride: the host rides -------------------------------------------------------------------------------------------------
// The host's own lift after the press: its stop or its reversal ends the ride, as the client's watch is told.
void HostRide(void* lift) {
    bool moving = false, dir = false;
    if (!CL::ReadMoving(lift, moving) || !CL::ReadDir(lift, dir)) return Invalid("the lift's state stopped reading during the ride");
    if (moving && !g_sawMoving) {
        g_sawMoving = true;
        g_prevDir = dir;
    }
    const bool flipped = g_sawMoving && dir != g_prevDir;
    g_prevDir = dir;
    if (flipped || (g_sawMoving && !moving)) {
        Fact("the host's ride ended (%s)", flipped ? "the lift reversed" : "the lift arrived");
        Say("end");
        HGo(HS::WaitResult);
        return;
    }
    if (!g_sawMoving) Late("the lift never began to move after the press", kStartBoundMs);
    else Late("the lift moved and did not stop", kRideBoundMs);
}

// The client's reading of the host's puppet: its tallies, in the order it says them.
void WaitResult() {
    std::string rest;
    if (!Rest("result ", &rest)) {
        Late("the client's reading of the host's puppet never came");
        return;
    }
    std::istringstream in(rest);
    int frames = 0, flips = 0, based = 0, attached = 0;
    float maxD = 0.f, maxFeet = 0.f, maxGap = 0.f;
    if (!(in >> frames >> flips >> maxD >> maxFeet >> based >> attached >> maxGap))
        return Invalid("the client's reading of the host's puppet could not be read");
    // The client fails the arm at the first frame its feet left the slab by more than kFeetTolCm (measured: 2.2 cm green, 96.5 cm
    // with the base off), so a result that reaches here held the limit on every frame; a pose that never named a base judged nothing.
    if (frames <= 0 || based <= 0) return Invalid("the host's pose never named a base on the client's copy: nothing was judged");
    if (maxFeet > kFeetTolCm) return Fail("the host's puppet left the slab by %.1f cm on the client's copy (limit %.0f)", maxFeet, kFeetTolCm);
    Pass("the lift reached its stop with the host riding: the host's puppet stayed within %.1f cm of the slab on the client's copy "
         "(limit %.0f) over %d frames, %d with a base, attached %d, reversals %d, max d=%.1f, max |attach-comp|=%.1f", maxFeet,
         kFeetTolCm, frames, based, attached, flips, maxD, maxGap);
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
    case HS::Hostile:
        if (!lift) return Invalid("the host's lift was lost");
        JudgeForged(s, lift);
        return;
    case HS::Board: {
        void* const me = coop::players::Registry::Get().Local();
        if (!lift || !me) return;
        switch (g_boarder.Tick(me)) {
        case Boarder::State::OnPlatform:
            Say("hostonboard");
            HGo(HS::WaitSeen);
            return;
        case Boarder::State::NoRoute:
            BringOther(lift);
            return;
        default:
            return;
        }
    }
    case HS::WaitSeen:
        if (!lift) return Invalid("the host's lift was lost");
        if (!Seen("seen")) {
            Late("the client never saw the host's puppet on its platform", kBoundMs);
            return;
        }
        if (!CL::Press(lift)) return Invalid("the lift's own press did not run");
        Fact("pressed");
        g_sawMoving = false;
        HGo(HS::HostRide);
        return;
    case HS::HostRide:
        if (!lift) return Invalid("the host's lift was lost");
        HostRide(lift);
        return;
    case HS::EndJudge:
        JudgeEnd();
        return;
    case HS::WaitResult:
        WaitResult();
        return;
    }
}

}  // namespace coop::dev::basedmove_drill::detail
