// coop/player/hit_judge.cpp -- see coop/player/hit_judge.h.

#include "coop/player/hit_judge.h"

#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/weapon_catalog.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace coop::hit_judge {
namespace {

namespace E = ue_wrap::engine;
using ue_wrap::FVector;

float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

bool JudgeHit(int slot, const FVector& eye, const FVector& hitLoc, void* target, float claimedDamage,
              float& outDamage, const char*& outReason) {
    outDamage = 0.f;
    ue_wrap::weapon_catalog::Swing swing;
    if (!ue_wrap::weapon_catalog::Lookup(coop::hand_item::HeldItem(static_cast<uint8_t>(slot)), swing)) {
        outReason = "weapon table unreadable";
        return false;
    }
    if (!swing.canSwing) {
        outReason = "no swinging item";
        return false;
    }
    // Written so that a NaN fails.
    if (!(Dist(eye, hitLoc) <= swing.length + kReachSlackCm)) {
        outReason = "out of reach";
        return false;
    }

    // Players are Pawns and so stand outside the {WorldStatic, WorldDynamic} set: a body between the eye and the point
    // is not cover. The puppet is the trace's world context and is ignored as its own actor.
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    void* puppet = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    const uint8_t types[2] = {0, 1};   // WorldStatic, WorldDynamic
    ue_wrap::trace::Hit blocker;
    if (!puppet || !ue_wrap::trace::LineHitForObjects(puppet, eye, hitLoc, types, 2, nullptr, 0, &blocker)) {
        outReason = "sight unreadable";
        return false;
    }
    // A block within kBlockedSlackCm of the point still reaches it: a hit point at a creature's feet sits in the floor.
    // Written so that a NaN blocks.
    if (blocker.blocked && blocker.actor != target &&
        !(Dist(eye, blocker.point) >= Dist(eye, hitLoc) - kBlockedSlackCm)) {
        outReason = "no sight";
        return false;
    }

    FVector at{};
    if (!E::TryGetActorLocation(target, at)) {
        outReason = "target unreadable";
        return false;
    }
    if (!(Dist(hitLoc, at) <= kTargetRadiusCm)) {
        outReason = "not on target";
        return false;
    }

    outDamage = claimedDamage < swing.maxDamage ? claimedDamage : swing.maxDamage;
    outReason = nullptr;
    return true;
}

}  // namespace coop::hit_judge
