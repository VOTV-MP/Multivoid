// coop/dev/drill_place.cpp -- see drill_place.h.
//
// Why the spots are fixed: a walk drill starts its walk from wherever the local player stands after the join, and that
// place comes from the save it loads (the rig's "heelloo" save, VotV/Saved/SaveGames/heelloo). The verified walks start
// from (-535, 310, 6665) as a client (the join's teleport_client "teleportWObackrooms") and from (-1692, 560, 6318) as
// the host. The join now lands the client at (-2405, 856, 6163) and the host starts at (5608, 818, 6180); walks from
// there found no route or stalled on the landscape and timed out. The drill places the pawn at the verified spot; what
// the drill verifies is untouched.

#include "coop/dev/drill_place.h"

#include "coop/session/teleport_client.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>

namespace coop::dev::drill_place {
namespace {

namespace E = ue_wrap::engine;

constexpr float    kLandFlatCm = 150.f;
constexpr float    kLandUpCm   = 200.f;
constexpr uint64_t kLandBoundMs = 15000;

}  // namespace

Result Run(Placement& p, void* pawn, const Spot& spot, uint64_t nowMs, const char* what) {
    if (!pawn) {
        UE_LOGW("[DRILL-PLACE] %s: the local player does not read -- no placement", what);
        return Result::Failed;
    }
    if (!p.issued) {
        p.issued = true;
        p.atMs = nowMs;
        if (!coop::teleport_client::ApplyLocally({spot.x, spot.y, spot.z, 0.f, spot.yaw, 0.f})) {
            UE_LOGW("[DRILL-PLACE] %s: the teleport to (%.0f, %.0f, %.0f) reported failure", what, spot.x, spot.y, spot.z);
            return Result::Failed;
        }
        UE_LOGI("[DRILL-PLACE] %s: the puppet is teleported to (%.0f, %.0f, %.0f) before the drill's walk", what, spot.x,
                spot.y, spot.z);
        return Result::Pending;
    }
    ue_wrap::FVector now{};
    if (E::TryGetActorLocation(pawn, now)) {
        const float flat = std::hypot(now.X - spot.x, now.Y - spot.y);
        const float up = std::fabs(now.Z - spot.z);
        if (flat <= kLandFlatCm && up <= kLandUpCm) {
            UE_LOGI("[DRILL-PLACE] %s: landed at (%.0f, %.0f, %.0f), %.0f cm from the spot", what, now.X, now.Y, now.Z,
                    flat);
            return Result::Landed;
        }
    }
    if (nowMs - p.atMs < kLandBoundMs) return Result::Pending;
    UE_LOGW("[DRILL-PLACE] %s: the pawn did not land on the spot within %llu ms", what,
            static_cast<unsigned long long>(kLandBoundMs));
    return Result::Failed;
}

}  // namespace coop::dev::drill_place
