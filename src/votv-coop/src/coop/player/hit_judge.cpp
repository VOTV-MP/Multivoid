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

// The cosine of the angle, in 3-D, between the unit vector `aim` and the direction from `eye` to `hitLoc`. 1 when
// `hitLoc` lies within kFacingMinCm of `eye` (no direction to test). NaN when an input is, so a caller written
// `!(c >= bound)` refuses it.
float FacingCosine(const FVector& aim, const FVector& eye, const FVector& hitLoc) {
    const float dx = hitLoc.X - eye.X, dy = hitLoc.Y - eye.Y, dz = hitLoc.Z - eye.Z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len <= kFacingMinCm) return 1.f;
    return (aim.X * dx + aim.Y * dy + aim.Z * dz) / len;
}

}  // namespace

bool JudgeHit(int slot, const FVector& eye, const FVector& aim, const FVector& hitLoc, void* target,
              float claimedDamage, float& outDamage, const char*& outReason) {
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
    if (!puppet) {
        outReason = "puppet unreadable";
        return false;
    }
    // No liveness gate: no death state reaches the host (a peer's death never sets the puppet's `dead`), and a
    // client's dead player makes no swing of its own. MTA refuses a dead shooter on its server's own spawn and death
    // state (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CBulletsyncPacket.cpp:165).

    // Written so that a NaN fails.
    if (!(Dist(eye, hitLoc) <= swing.length + kReachSlackCm)) {
        outReason = "out of reach";
        return false;
    }
    // Source's melee refuses a struck target the swinger is not "sort of facing" by its aim vector
    // (reference/source-sdk-2013/src/game/shared/hl2mp/weapon_hl2mpbasebasebludgeon.cpp:298, :310-331); the host holds
    // the aim the puppet streamed and the claimed point, and bounds the one against the other in 3-D. Always on, with
    // the aim captured at the intent's arrival beside the eye.
    if (!(FacingCosine(aim, eye, hitLoc) >= kFacingCosine)) {
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

    // A ceiling, not a recomputation: the host does not read the struck material (that needs a trace against the
    // creature's mesh, and the host's row holds the multipliers), so it bounds the claim by the item's most (damage
    // times its largest multiplier). Both precedents compute the damage on the authority: Source from the server's
    // weapon data (weapon_hl2mpbasebasebludgeon.cpp:149-165), MTA's bulletsync damage being only a backup
    // (CClientPlayer.cpp:260 fBackupDamage).
    outDamage = claimedDamage < swing.maxDamage ? claimedDamage : swing.maxDamage;
    outReason = nullptr;
    return true;
}

}  // namespace coop::hit_judge
