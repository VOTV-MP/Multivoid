// harness/autotest/level_ground.h -- TEST-ONLY: level open ground round the host, for a drill that sets a machine down.
//
// Co-located private header of the drills in this folder. Host, from a drill's thread.

#pragma once

#include "disc_drills.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace harness::autotest::level_ground {

using ue_wrap::FVector;

// Open, level ground round the host: `envKey` ("x,y,z") when given; else papers are dropped round the host,
// 40 and 60 m off at eight bearings, and the first that lands and lies still (it moved under 50 cm across, then under
// 2 cm in a second) names the spot. A paper on a slope slides off; one indoors lands on a floor, which the upright test
// of the chipper then catches. The test papers are removed. Host, from the drill's thread.
inline bool Find(FVector& at, const char* envKey) {
    const std::string env = coop::config::ReadEnv(envKey);
    if (!env.empty()) return std::sscanf(env.c_str(), "%f,%f,%f", &at.X, &at.Y, &at.Z) == 3;
    constexpr int kN = 16;
    void* probe[kN] = {};
    FVector drop[kN] = {}, a[kN] = {}, b[kN] = {};
    disc_drill::OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/prop_paper.prop_paper_C");
        FVector ml{};
        if (!me || !cls || !ue_wrap::engine::TryGetActorLocation(me, ml)) return;
        for (int i = 0; i < kN; ++i) {
            const float r = i < 8 ? 4000.f : 6000.f, t = 0.7854f * static_cast<float>(i % 8);
            drop[i] = FVector{ml.X + r * std::cos(t), ml.Y + r * std::sin(t), ml.Z + 300.f};
            probe[i] = ue_wrap::engine::SpawnActor(cls, drop[i]);
        }
    });
    ::Sleep(5000);
    disc_drill::OnGameThread([&] {
        for (int i = 0; i < kN; ++i)
            if (probe[i] && ue_wrap::reflection::IsLive(probe[i])) ue_wrap::engine::TryGetActorLocation(probe[i], a[i]);
    });
    ::Sleep(1000);
    int pick = -1;
    disc_drill::OnGameThread([&] {
        for (int i = 0; i < kN; ++i) {
            if (!probe[i] || !ue_wrap::reflection::IsLive(probe[i])) continue;
            ue_wrap::engine::TryGetActorLocation(probe[i], b[i]);
            const float sx = b[i].X - drop[i].X, sy = b[i].Y - drop[i].Y;
            const bool level = std::sqrt(sx * sx + sy * sy) < 50.f && std::sqrt((a[i].X - b[i].X) * (a[i].X - b[i].X) + (a[i].Y - b[i].Y) * (a[i].Y - b[i].Y) + (a[i].Z - b[i].Z) * (a[i].Z - b[i].Z)) < 2.f && b[i].Z < drop[i].Z - 20.f;
            UE_LOGI("level-ground: HOST probe %d at %.0f,%.0f: rests at z %.0f, slid %.0f cm%s", i, drop[i].X,
                    drop[i].Y, b[i].Z, std::sqrt(sx * sx + sy * sy), level ? " -- level" : "");
            if (level && pick < 0) pick = i;
        }
        for (int i = 0; i < kN; ++i)
            if (probe[i] && ue_wrap::reflection::IsLive(probe[i])) ue_wrap::engine::DestroyActor(probe[i]);
    });
    if (pick < 0) return false;
    at = FVector{b[pick].X, b[pick].Y, b[pick].Z + 120.f};
    return true;
}

}  // namespace harness::autotest::level_ground
