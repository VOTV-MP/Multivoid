// coop/props/toolgun_sight.cpp -- see coop/props/toolgun_sight.h.

#include "coop/props/toolgun_sight.h"

#include "coop/element/intent_authority.h"  // kPoseStalenessUU
#include "coop/player/players_registry.h"
#include "coop/props/toolgun_limits.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace coop::toolgun_sight {
namespace {

namespace E = ue_wrap::engine;
using ue_wrap::FVector;

inline float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

SightResult Fail(bool judged, const char* why) {
    SightResult r;
    r.judged = judged;
    r.why = why;
    return r;
}

}  // namespace

SightResult JudgeToolgunSight(void* puppet, void* actor, const FVector& origin, const FVector& hitAt,
                              const FVector& eye) {
    // The client's origin is only a consistency check against the eye the host captured when the request arrived.
    if (!(Dist(origin, eye) <= coop::element::kPoseStalenessUU))   // written so that a NaN fails
        return Fail(true, "origin far from the eye");
    // The hit point on the prop: inside the sphere of the bounds of ALL its components, plus a margin. A zero extent is
    // no bounds, never a point-sized prop at the world origin.
    FVector centre{}, extent{};
    float radius = 0.f;
    if (E::GetActorBounds(actor, /*onlyColliding=*/false, centre, extent) &&
        !(extent.X == 0.f && extent.Y == 0.f && extent.Z == 0.f)) {
        radius = std::sqrt(extent.X * extent.X + extent.Y * extent.Y + extent.Z * extent.Z);
    } else if (!E::TryGetActorLocation(actor, centre)) {
        return Fail(false, "the prop's bounds and location did not read");
    }
    if (!(Dist(hitAt, centre) <= radius + coop::toolgun_limits::kOnPropSlackCm))
        return Fail(true, "hit point off the prop");
    // The segment from the eye to the hit point meets nothing the shot itself would stop at (the toolgun traces these
    // four object types) before it, but the players' bodies and the prop.
    const uint8_t types[4] = {0, 1, 2, 3};   // the shot's own set: WorldStatic, WorldDynamic, Pawn, PhysicsBody
    // Players are people, not cover: every player body (this process's own and each puppet) is ignored, with the prop. A
    // body that holds the sender's eye -- two players standing at one spot -- would otherwise block at no distance. The
    // host is deliberately more lenient here than the native shot, which traces the same four types ignoring only its
    // shooter and so stops at another player's capsule. A modified client gains only the sight through a standing player,
    // which is mobile cover rather than geometry; the eye ball and the prop's sphere still bound the claim.
    void* ignore[coop::players::kMaxPeers + 1];
    int nIgnore = coop::players::Registry::Get().LiveBodies(ignore, coop::players::kMaxPeers);
    ignore[nIgnore++] = actor;
    ue_wrap::trace::Hit blocker;
    if (!ue_wrap::trace::LineHitForObjects(puppet, eye, hitAt, types, 4, ignore, nIgnore, &blocker))
        return Fail(false, "the sight trace did not run");
    if (blocker.blocked &&
        !(Dist(eye, blocker.point) >= Dist(eye, hitAt) - coop::toolgun_limits::kBlockedSlackCm)) {
        SightResult r = Fail(true, "segment blocked before the hit point");
        r.blocked = true;
        r.blockerActor = blocker.actor;
        r.dEyeBlock = Dist(eye, blocker.point);
        r.dEyeHit = Dist(eye, hitAt);
        r.dOriginEye = Dist(origin, eye);
        return r;
    }
    SightResult ok;
    ok.ok = true;
    return ok;
}

void NoteBlocked(const char* lane, coop::net::RefusalTally::Clock::time_point& last, const SightResult& s) {
    const auto now = coop::net::RefusalTally::Clock::now();
    if (last != coop::net::RefusalTally::Clock::time_point{} && now - last < coop::net::RefusalTally::kWindow) return;
    last = now;
    const std::wstring cls = s.blockerActor ? ue_wrap::reflection::ClassNameOf(s.blockerActor) : std::wstring(L"(no actor)");
    UE_LOGI("%s: a segment was blocked by %ls at %.0f uu from the eye; the hit point is %.0f uu away and the client's "
            "origin %.0f uu from the eye", lane, cls.c_str(), s.dEyeBlock, s.dEyeHit, s.dOriginEye);
}

}  // namespace coop::toolgun_sight
