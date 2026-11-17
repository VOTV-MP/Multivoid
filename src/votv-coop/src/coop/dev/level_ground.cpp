// coop/dev/level_ground.cpp -- see coop/dev/level_ground.h.

#include "coop/dev/level_ground.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/trace.h"

#include <cmath>

namespace coop::dev::level_ground {

using ue_wrap::FVector;

bool FindFrom(void* walker, const FVector& around, FVector& at, Accept accept, void* ctx) {
    constexpr float kHalfCm = 300.f;
    constexpr float kFlatCm = 5.f;
    constexpr float kUprightZ = 0.9986f;
    constexpr float kRingsCm[] = {3000.f, 4500.f, 6000.f, 8000.f, 10000.f};
    constexpr int kBearings = 16;
    constexpr float kReachUp = 1500.f, kReachDown = 2500.f;
    constexpr float kOff[9][2] = {{0, 0}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    int tried = 0, noFloor = 0;
    float bestSpread = 1e9f;
    FVector bestAt{};
    for (float ring : kRingsCm) {
        for (int b = 0; b < kBearings; ++b) {
            const float t = 6.2831853f * static_cast<float>(b) / static_cast<float>(kBearings);
            const float cx = around.X + ring * std::cos(t), cy = around.Y + ring * std::sin(t);
            float zMin = 1e9f, zMax = -1e9f, centreZ = 0.f, centreNormalZ = 0.f;
            bool all = true;
            int missed = -1;
            for (int i = 0; i < 9 && all; ++i) {
                ue_wrap::trace::Hit h;
                const float x = cx + kOff[i][0] * kHalfCm, y = cy + kOff[i][1] * kHalfCm;
                if (!ue_wrap::trace::LineHitStatDyn(walker, FVector{x, y, around.Z + kReachUp},
                                                    FVector{x, y, around.Z - kReachDown}, &h) ||
                    !h.blocked) {
                    all = false;
                    missed = i;
                    break;
                }
                zMin = std::fmin(zMin, h.point.Z);
                zMax = std::fmax(zMax, h.point.Z);
                if (i == 0) {
                    centreZ = h.point.Z;
                    centreNormalZ = h.normal.Z;
                }
            }
            ++tried;
            if (!all) {
                ++noFloor;
                UE_LOGI("level-ground: HOST candidate %d at %.0f,%.0f, %.0f m out: refused -- no floor under trace %d of 9 "
                        "(%.0f cm above the walker down to %.0f cm below)", tried, cx, cy, ring / 100.f, missed, kReachUp,
                        kReachDown);
                continue;
            }
            const float spread = zMax - zMin;
            if (spread < bestSpread) {
                bestSpread = spread;
                bestAt = FVector{cx, cy, centreZ};
            }
            if (spread <= kFlatCm && centreNormalZ >= kUprightZ) {
                const FVector floorAt{cx, cy, centreZ};
                if (accept && !accept(ctx, walker, floorAt)) {
                    UE_LOGI("level-ground: HOST candidate %d at %.0f,%.0f, %.0f m out: refused -- level, but the caller does "
                            "not accept it", tried, cx, cy, ring / 100.f);
                    continue;
                }
                at = floorAt;
                UE_LOGI("level-ground: HOST floor at %.0f,%.0f,%.0f, %.0f m out: nine traces over %.0f cm span within %.1f cm, "
                        "centre normal z %.4f (candidate %d)", cx, cy, centreZ, ring / 100.f, kHalfCm * 2.f, spread,
                        centreNormalZ, tried);
                return true;
            }
            UE_LOGI("level-ground: HOST candidate %d at %.0f,%.0f, %.0f m out: refused -- %s (nine heights span %.1f cm, "
                    "limit %.0f; centre normal z %.4f, limit %.4f)", tried, cx, cy, ring / 100.f,
                    spread > kFlatCm ? "the floor is not level" : "the centre is not flat", spread, kFlatCm, centreNormalZ,
                    kUprightZ);
        }
    }
    UE_LOGW("level-ground: HOST no level floor in %d candidates (%d had no floor under a trace); flattest spread %.1f cm "
            "at %.0f,%.0f,%.0f", tried, noFloor, bestSpread, bestAt.X, bestAt.Y, bestAt.Z);
    return false;
}

}  // namespace coop::dev::level_ground
