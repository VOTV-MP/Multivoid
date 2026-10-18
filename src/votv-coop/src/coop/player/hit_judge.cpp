// coop/player/hit_judge.cpp -- see coop/player/hit_judge.h.

#include "coop/player/hit_judge.h"

#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
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

// The cosine of the angle, in the horizontal plane, between the puppet's facing (its actor yaw on the host) and the
// direction from `eye` to `hitLoc`. NaN when either has no horizontal extent, so a caller written `!(c >= bound)`
// refuses it.
float FacingCosine(float yawDegrees, const FVector& eye, const FVector& hitLoc) {
    const float rad = yawDegrees * 0.01745329252f;
    const float fx = std::cos(rad), fy = std::sin(rad);
    const float dx = hitLoc.X - eye.X, dy = hitLoc.Y - eye.Y;
    const float len = std::sqrt(dx * dx + dy * dy);
    return (fx * dx + fy * dy) / len;
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
    if (!(swing.length > 0.f)) {
        outReason = "no reach data";
        return false;
    }

    // Players are Pawns and so stand outside the {WorldStatic, WorldDynamic} set: a body between the eye and the point
    // is not cover. The puppet is the trace's world context and is ignored as its own actor.
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    void* puppet = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    bool ragdoll = false, dead = false;
    ue_wrap::FRotator facing{};
    if (!puppet || !E::ReadMainPlayerRagdollState(puppet, ragdoll, dead) || !E::TryGetActorRotation(puppet, facing)) {
        outReason = "puppet unreadable";
        return false;
    }
    // MTA refuses a shot from a shooter that is not spawned and alive
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CBulletsyncPacket.cpp:165).
    if (dead) {
        outReason = "dead";
        return false;
    }

    // Written so that a NaN fails.
    if (!(Dist(eye, hitLoc) <= swing.length + kReachSlackCm)) {
        outReason = "out of reach";
        return false;
    }
    // Source's melee refuses a struck target the swinger is not "sort of facing"
    // (reference/source-sdk-2013/src/game/shared/hl2mp/weapon_hl2mpbasebasebludgeon.cpp:292-331); the host holds the
    // puppet's facing and the claimed point, and bounds the one against the other the same way.
    if (!(FacingCosine(facing.Yaw, eye, hitLoc) >= kFacingCosine)) {
        outReason = "not facing";
        return false;
    }

    const uint8_t types[2] = {0, 1};   // WorldStatic, WorldDynamic
    ue_wrap::trace::Hit blocker;
    if (!ue_wrap::trace::LineHitForObjects(puppet, eye, hitLoc, types, 2, nullptr, 0, &blocker)) {
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

    // A ceiling, not a recomputation: the struck material's multiplier is the client's to know, so the host cannot
    // derive the damage and bounds the claim by the item's most (damage times its largest multiplier).
    outDamage = claimedDamage < swing.maxDamage ? claimedDamage : swing.maxDamage;
    outReason = nullptr;
    return true;
}

}  // namespace coop::hit_judge
