// harness/autotest/level_ground.h -- TEST-ONLY: level open ground round the host, for a drill that sets a machine down.
//
// Co-located private header of the drills in this folder. Host, from a drill's thread.

#pragma once

#include "disc_drills.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace harness::autotest::level_ground {

using ue_wrap::FVector;

// A spot whose whole footprint is level, measured: nine line traces from above (the centre, the four corners and the four
// edge midpoints of a square 2 * kHalfCm across) must all strike a floor, within kFlatCm of one another in height, with
// the centre's surface normal within 3 degrees of vertical. A body that settles on such a square stands upright, whatever
// its base: a paper that merely rests where it fell proves nothing, since a paper rests on a 5 degree slope. Candidates
// are tried nearest first, 16 bearings to a ring, and the first that passes is the answer. `at` is the floor at the
// centre. `envKey` ("x,y,z"), when set, names that floor point instead and is not measured. Host, from the drill's thread.
inline bool Find(FVector& at, const char* envKey) {
    const std::string env = coop::config::ReadEnv(envKey);
    if (!env.empty()) return sscanf_s(env.c_str(), "%f,%f,%f", &at.X, &at.Y, &at.Z) == 3;
    constexpr float kHalfCm = 300.f;
    constexpr float kFlatCm = 5.f;
    constexpr float kUprightZ = 0.9986f;
    constexpr float kRingsCm[] = {3000.f, 4500.f, 6000.f, 8000.f, 10000.f};
    constexpr int kBearings = 16;
    constexpr float kReachUp = 1500.f, kReachDown = 2500.f;
    constexpr float kOff[9][2] = {{0, 0}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    bool found = false;
    int tried = 0, noFloor = 0;
    float bestSpread = 1e9f;
    FVector bestAt{};
    disc_drill::OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        FVector ml{};
        if (!me || !ue_wrap::engine::TryGetActorLocation(me, ml)) return;
        for (float ring : kRingsCm) {
            for (int b = 0; b < kBearings; ++b) {
                const float t = 6.2831853f * static_cast<float>(b) / static_cast<float>(kBearings);
                const float cx = ml.X + ring * std::cos(t), cy = ml.Y + ring * std::sin(t);
                float zMin = 1e9f, zMax = -1e9f, centreZ = 0.f, centreNormalZ = 0.f;
                bool all = true;
                int missed = -1;
                for (int i = 0; i < 9 && all; ++i) {
                    ue_wrap::trace::Hit h;
                    const float x = cx + kOff[i][0] * kHalfCm, y = cy + kOff[i][1] * kHalfCm;
                    if (!ue_wrap::trace::LineHitStatDyn(me, FVector{x, y, ml.Z + kReachUp}, FVector{x, y, ml.Z - kReachDown}, &h) ||
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
                    at = FVector{cx, cy, centreZ};
                    found = true;
                    UE_LOGI("level-ground: HOST floor at %.0f,%.0f,%.0f, %.0f m out: nine traces over %.0f cm span within %.1f cm, "
                            "centre normal z %.4f (candidate %d)", cx, cy, centreZ, ring / 100.f, kHalfCm * 2.f, spread,
                            centreNormalZ, tried);
                    return;
                }
                UE_LOGI("level-ground: HOST candidate %d at %.0f,%.0f, %.0f m out: refused -- %s (nine heights span %.1f cm, "
                        "limit %.0f; centre normal z %.4f, limit %.4f)", tried, cx, cy, ring / 100.f,
                        spread > kFlatCm ? "the floor is not level" : "the centre is not flat", spread, kFlatCm, centreNormalZ,
                        kUprightZ);
            }
        }
    });
    if (!found)
        UE_LOGW("level-ground: HOST no level floor in %d candidates (%d had no floor under a trace); flattest spread %.1f cm "
                "at %.0f,%.0f,%.0f", tried, noFloor, bestSpread, bestAt.X, bestAt.Y, bestAt.Z);
    return found;
}

}  // namespace harness::autotest::level_ground
