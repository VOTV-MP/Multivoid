// coop/player/shelter_spot.cpp -- see coop/player/shelter_spot.h.

#include "coop/player/shelter_spot.h"

#include "coop/config/config.h"
#include "ue_wrap/actors/sleep.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"

#include <atomic>
#include <cmath>

namespace coop::shelter_spot {
namespace {

namespace E = ue_wrap::engine;
namespace P = ue_wrap::profile;

// The NavMesh runs UNDER the bed (the bed does not cut it; measured on the rig: the point nearest the
// bed's location was the bed's own XY, and a player put there stood inside the mattress and could not
// move). So the spot is chosen outside the bed's colliding box: a point past each side by the player's
// capsule radius and a clearance, projected onto the NavMesh, and kept only if its projection still
// lies clear of the box grown by the radius. The query box is small horizontally, so a projection
// cannot slide back under the bed; 250 vertical covers the bed's pivot sitting above or below the floor.
constexpr float kNavExtentHorizCm = 40.f;
constexpr float kNavExtentVertCm = 250.f;
// The gap left between the capsule and the bed's box, so the body does not start touching it.
constexpr float kClearanceCm = 25.f;

struct Side { float dx, dy; };

// A candidate beside the box on side `d` (a unit axis or diagonal), and whether its projection is clear.
bool TrySide(void* pawn, const ue_wrap::FVector& o, const ue_wrap::FVector& ext, float radius, Side d,
             ue_wrap::FVector& out) {
    // How far the box reaches along d (its support), then the capsule and the clearance past it.
    const float reach = std::fabs(d.dx) * ext.X + std::fabs(d.dy) * ext.Y + radius + kClearanceCm;
    const ue_wrap::FVector at{o.X + d.dx * reach, o.Y + d.dy * reach, o.Z};
    ue_wrap::FVector floor{};
    if (!E::ProjectToNav(pawn, at, {kNavExtentHorizCm, kNavExtentHorizCm, kNavExtentVertCm}, &floor))
        return false;
    const bool insideX = std::fabs(floor.X - o.X) < ext.X + radius;
    const bool insideY = std::fabs(floor.Y - o.Y) < ext.Y + radius;
    if (insideX && insideY) return false;
    out = floor;
    return true;
}

std::atomic<bool> g_fallbackLogged{false};

Target Fallback(const char* why) {
    if (!g_fallbackLogged.exchange(true, std::memory_order_relaxed))
        UE_LOGW("shelter_spot: falling back to the KPP (open sky) -- %s", why);
    return {Kind::KppFallback, P::name::kKPPSpawnX, P::name::kKPPSpawnY, P::name::kKPPSpawnZ};
}

}  // namespace

const char* KindName(Kind k) {
    return k == Kind::Bed ? "bed" : k == Kind::KppFallback ? "kpp-fallback" : "none";
}

Target Choose(void* pawn) {
    if (BedLookupDisabled()) return Fallback("VOTVCOOP_DEATH_NO_BED=1, the bed lookup is switched off");
    void* const bed = ue_wrap::sleep::NearestBed({P::name::kKPPSpawnX, P::name::kKPPSpawnY, P::name::kKPPSpawnZ});
    if (!bed) return Fallback("no bed in the world");
    ue_wrap::FVector o{}, ext{};
    if (!E::GetActorBounds(bed, /*onlyColliding*/ true, o, ext) || ext.X <= 0.f || ext.Y <= 0.f)
        return Fallback("the bed's colliding bounds could not be read");
    const float halfH = E::GetActorCharacterHalfHeight(pawn);
    const float radius = E::GetActorCharacterRadius(pawn);
    if (halfH <= 0.f || radius <= 0.f) return Fallback("the player's capsule could not be read");
    // The long sides first (a bed is entered from its side), then its ends, then the corners.
    constexpr float kD = 0.70710678f;
    const bool longAlongY = ext.Y >= ext.X;
    const Side sides[8] = {
        longAlongY ? Side{1, 0} : Side{0, 1}, longAlongY ? Side{-1, 0} : Side{0, -1},
        longAlongY ? Side{0, 1} : Side{1, 0}, longAlongY ? Side{0, -1} : Side{-1, 0},
        {kD, kD}, {-kD, kD}, {kD, -kD}, {-kD, -kD}};
    for (int i = 0; i < 8; ++i) {
        ue_wrap::FVector floorAt{};
        if (!TrySide(pawn, o, ext, radius, sides[i], floorAt)) continue;
        UE_LOGI("shelter_spot: bed %p box (%.0f,%.0f,%.0f) +-(%.0f,%.0f) -> side %d floor (%.0f,%.0f,%.0f), "
                "capsule r=%.0f half-height %.0f", bed, o.X, o.Y, o.Z, ext.X, ext.Y, i, floorAt.X, floorAt.Y,
                floorAt.Z, radius, halfH);
        return {Kind::Bed, floorAt.X, floorAt.Y, floorAt.Z + halfH};
    }
    return Fallback("no NavMesh point clear of the bed's box on any side");
}

bool BedLookupDisabled() {
    static const bool s = (coop::config::ReadEnv("VOTVCOOP_DEATH_NO_BED") == "1");
    return s;
}

void OnSessionStart() { g_fallbackLogged.store(false, std::memory_order_relaxed); }

}  // namespace coop::shelter_spot
