// coop/world/power_grid_rows.h -- the base's generators as wire rows, beside coop/world/power_grid: reading them off the
// game's generators and comparing two readings. Game thread.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::power_grid::row_reading {

// The insert's own gate, `upgradeLevel < 6`: a generator's upgrades are full at this level.
inline constexpr int32_t kMaxUpgrade = 6;

// Fill `p` with the generators' rows in gamemode.generators order, capped at kPowerGridGenerators, each with its repair
// puzzle. False when the game holds no generators yet (nothing is written then).
bool ReadRows(coop::net::PowerGridPayload& p);

// Whether two readings carry the same rows: the count and, per row, every field the wire carries.
bool SameRows(const coop::net::PowerGridPayload& a, const coop::net::PowerGridPayload& b);

}  // namespace coop::power_grid::row_reading
