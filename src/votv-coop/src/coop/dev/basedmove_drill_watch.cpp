// coop/dev/basedmove_drill_watch.cpp -- one frame of a puppet riding a cargo lift copy, for the based-movement drill; see
// coop/dev/basedmove_drill.h. The host watches the client's puppet (ride), the client the host's (hostride): each against its OWN
// copy of the lift, read as the lift's own block test reads it.
//
// Per frame: the gap the Blueprint's reversal reads (attach Z less platform Z; it reverses at 10 cm unless ignoreCheck holds),
// the distance d from where the puppet stands to where its latest pose puts it (the platform's location now plus the rider's own
// offset), its feet against the slab's surface, and what its root is attached to. A reversal is `dir` changing once the lift
// has moved (the press itself flips it before).

#include "basedmove_drill_detail.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/actors/character_base.h"
#include "ue_wrap/devices/cargo_lift.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_component.h"

#include <cmath>
#include <string>

namespace coop::dev::basedmove_drill::detail {

namespace E  = ue_wrap::engine;
namespace CL = ue_wrap::cargo_lift;
namespace CB = ue_wrap::character_base;

Watched Watcher::Frame(coop::net::Session& s, void* lift, const PlatformBox& box) {
    Watched w;
    bool moving = false, dir = false, ign = false;
    float platZ = 0.f, attZ = 0.f;
    if (!CL::ReadMoving(lift, moving) || !CL::ReadDir(lift, dir) || !CL::ReadIgnoreCheck(lift, ign) ||
        !CL::ReadPlatformZ(lift, platZ) || !CL::ReadAttachZ(lift, attZ))
        return w;
    coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot_));
    void* const body = rp && rp->valid() && rp->HasPose() ? rp->GetActor() : nullptr;
    void* const platform = CL::PlatformActor(lift);
    ue_wrap::FVector at{}, origin{};
    if (!body || !platform || !E::TryGetActorLocation(body, at) || !E::TryGetActorLocation(platform, origin)) return w;

    w.ok = true;
    w.moving = moving;
    w.gap = attZ - platZ;
    if (moving && !sawMoving_) {   // the press flipped `dir` already: a reversal is a change from here
        sawMoving_ = true;
        prevDir_ = dir;
    }
    w.flipped = sawMoving_ && dir != prevDir_;
    if (w.flipped) ++flips;
    prevDir_ = dir;

    coop::net::PoseSnapshot pose{};
    w.based = s.TryGetRemotePose(slot_, pose) && pose.baseKind == coop::net::pose_base_kind::kMover;
    if (w.based) {
        const float dx = at.X - (origin.X + pose.bx), dy = at.Y - (origin.Y + pose.by), dz = at.Z - (origin.Z + pose.bz);
        w.d = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    w.at = at;
    w.feet = at.Z - E::GetActorCharacterHalfHeight(body) - (box.surface + platZ - box.restZ);
    void* parent = nullptr;
    w.attached = E::GetAttachParentActor(body, parent) && parent == platform;
    void* const cap = CB::CapsuleComponent(body);
    const bool overlap = cap && E::IsOverlappingActor(cap, platform);

    ++frames;
    maxGap = std::fmax(maxGap, std::fabs(w.gap));
    maxFeet = std::fmax(maxFeet, std::fabs(w.feet));
    if (w.based) {
        ++basedFrames;
        maxD = std::fmax(maxD, w.d);
    }
    if (w.attached) ++attachedFrames;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
    Fact("f=%d t=%.1fms platZ=%.2f attZ=%.2f attZ-platZ=%.2f dir=%d ignoreCheck=%d moving=%d flip=%d puppetZ=%.2f based=%d "
         "d=%.1f feetOverSurface=%.1f attached=%d capsuleColl=%d overlap=%d",
         frames, ms, platZ, attZ, w.gap, dir ? 1 : 0, ign ? 1 : 0, moving ? 1 : 0, w.flipped ? 1 : 0, at.Z, w.based ? 1 : 0, w.d,
         w.feet, w.attached ? 1 : 0, cap ? E::GetComponentCollisionEnabled(cap) : -1, overlap ? 1 : 0);
    return w;
}

}  // namespace coop::dev::basedmove_drill::detail
