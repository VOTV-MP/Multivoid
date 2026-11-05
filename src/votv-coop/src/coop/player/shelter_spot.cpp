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

namespace coop::shelter_spot {
namespace {

namespace E = ue_wrap::engine;
namespace P = ue_wrap::profile;

// The NavMesh query box around the bed's location. The bed's footprint is no NavMesh, so the nearest
// navigable point is the floor beside it; 250 vertical covers the bed's pivot sitting above or below
// the floor.
constexpr float kNavExtentHorizCm = 150.f;
constexpr float kNavExtentVertCm = 250.f;

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
    ue_wrap::FVector bedAt{};
    if (!E::TryGetActorLocation(bed, bedAt)) return Fallback("the bed's location could not be read");
    ue_wrap::FVector floorAt{};
    if (!E::ProjectToNav(pawn, bedAt, {kNavExtentHorizCm, kNavExtentHorizCm, kNavExtentVertCm}, &floorAt))
        return Fallback("no NavMesh point within the query box of the bed");
    const float halfH = E::GetActorCharacterHalfHeight(pawn);
    if (halfH <= 0.f) return Fallback("the player's capsule half-height could not be read");
    UE_LOGI("shelter_spot: bed %p at (%.0f,%.0f,%.0f) -> floor (%.0f,%.0f,%.0f), capsule half-height %.0f",
            bed, bedAt.X, bedAt.Y, bedAt.Z, floorAt.X, floorAt.Y, floorAt.Z, halfH);
    return {Kind::Bed, floorAt.X, floorAt.Y, floorAt.Z + halfH};
}

bool BedLookupDisabled() {
    static const bool s = (coop::config::ReadEnv("VOTVCOOP_DEATH_NO_BED") == "1");
    return s;
}

void OnSessionStart() { g_fallbackLogged.store(false, std::memory_order_relaxed); }

}  // namespace coop::shelter_spot
