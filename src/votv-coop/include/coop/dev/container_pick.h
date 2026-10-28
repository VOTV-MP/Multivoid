// coop/dev/container_pick.h -- [dev] the world container a drill takes from: near the local player, a nav route reaches
// it within the walk budget, and it holds enough records. Both peers rank the candidates by element id, which the
// peers share, so they pick the same one while they stand together. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace coop::dev::container_pick {

inline constexpr float kPickNearCm = 5000.f;   // the pick takes a container within this of the local player
inline constexpr float kStandCm    = 150.f;    // this near the container, a player stands where it can open it
inline constexpr int   kWalkMaxS   = 900;      // the walk's longest budget, in seconds

// The walker's budget, in seconds, for a route of this length; and whether the route fits the longest budget.
int WalkSeconds(float routeCm);
bool WalkFits(float routeCm);

struct Want {
    int minRecords = 2;
    // Containers this answers true for rank before the others; null ranks by element id alone.
    bool (*prefers)(void* actor) = nullptr;
};

struct Pick {
    uint32_t eid = 0;
    void* actor = nullptr;
    ue_wrap::FVector pos{};
    int records = 0;
    float routeCm = 0.f;   // the nav route's flat length from the local player; 0 on a fallback pick
    bool fallback = false; // no container within kPickNearCm was reached: the nearest qualifying one, at any distance
    int seen = 0;          // the world containers walked
    int qualified = 0;     // those holding enough records
};

// The first near container, preferred ones first, that a nav route from the local player reaches within the walk
// budget. With none near, or none reached, the nearest qualifying one, marked `fallback`. False when there is none
// yet or the local player's place does not read; `out.seen` and `out.qualified` are set then too.
bool PickContainer(const Want& want, Pick& out);

}  // namespace coop::dev::container_pick
