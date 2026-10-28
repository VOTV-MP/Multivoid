// coop/dev/grid_drill_walk.h -- [dev] the grid drill's walk, beside grid_drill.cpp: a client that must stand
// beside the drill's generator walks there by the director (a navmesh route, never a teleport) and measures that it
// arrived within the reach the host judges a generator op with. A client joins at the world's start point, far from
// the generators, so every arm that acts on the generator begins here. Src-local.

#pragma once

#include <cstdint>

namespace coop::dev::grid_drill {

enum class WalkState : uint8_t {
    Walking,
    Arrived,  // within the host's reach of the generator's Activate button, and the arrival line was printed
    Ended,    // no standpoint, or the walk ended out of reach: an ABANDONED line was printed (a fixture's fault)
};

// One tick of the walk to a standpoint on a ring about the drill's generator's Activate button. The first call asks
// the director's route; later calls poll the walk. Game thread.
WalkState WalkBesideGenerator(void* player);

// The walk starts again. Session end.
void WalkReset();

}  // namespace coop::dev::grid_drill
