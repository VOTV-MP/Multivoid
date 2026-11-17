// harness/autotest/level_ground.h -- TEST-ONLY: level open ground round the host, for a drill that sets a machine down.
//
// Co-located private header of the drills in this folder. Host, from a drill's thread. The measuring is
// coop/dev/level_ground, which runs on the game thread; this is its form for a drill's own thread.

#pragma once

#include "disc_drills.h"

#include "coop/config/config.h"
#include "coop/dev/level_ground.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/engine/engine.h"

#include <cstdio>
#include <string>

namespace harness::autotest::level_ground {

using ue_wrap::FVector;

// Level open ground round the host, measured (coop/dev/level_ground). `at` is the floor at the centre. `envKey` ("x,y,z"),
// when set, names that floor point instead and is not measured. Host, from the drill's thread.
inline bool Find(FVector& at, const char* envKey) {
    const std::string env = coop::config::ReadEnv(envKey);
    if (!env.empty()) return sscanf_s(env.c_str(), "%f,%f,%f", &at.X, &at.Y, &at.Z) == 3;
    bool found = false;
    disc_drill::OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        FVector ml{};
        if (!me || !ue_wrap::engine::TryGetActorLocation(me, ml)) return;
        found = coop::dev::level_ground::FindFrom(me, ml, at);
    });
    return found;
}

}  // namespace harness::autotest::level_ground
