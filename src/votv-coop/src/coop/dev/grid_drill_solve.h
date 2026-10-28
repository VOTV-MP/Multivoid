// coop/dev/grid_drill_solve.h -- [dev] the grid drill's puzzle-solve legs, beside grid_drill.cpp: a client standing
// beside the drill's generator, which the host broke, holds the host's puzzle, enters its panel, and either solves
// it and presses Activate (the host must judge the press on its own copy and repair), or sends a probe input and a
// burst of them (the host must take every one, within its rate). The caller starts the legs once the client
// stands within reach of the generator, and says the end. Src-local.

#pragma once

#include <cstdint>

namespace coop::dev::grid_drill {

// The flood's burst: more than one burst of the host's rate for inputs, fewer than a burst and a full queue, so every
// input is taken, some at the rate. The host's check reads it too.
inline constexpr int kFloodInputs = 40;

enum class SolveState : uint8_t {
    Running,
    Ended,     // a FAIL or an ABANDONED line was printed
    Repaired,  // the solved panel's Activate press was answered, and the generator reads whole here
    Flooded,   // the host answered every input of the flood
};

// Start the legs: the probe and the flood after the panel is entered when `flood`, else the solve and the press.
void SolveBegin(bool flood);

// One tick of the legs. Game thread.
SolveState SolveTick(void* player);

}  // namespace coop::dev::grid_drill
