// coop/world/power_grid_rows.cpp -- see coop/world/power_grid_rows.h.

#include "coop/world/power_grid_rows.h"

#include "coop/world/power_puzzle.h"  // the rows' second half: each generator's repair puzzle

#include "ue_wrap/devices/generator.h"

#include <algorithm>
#include <vector>

namespace coop::power_grid::row_reading {

namespace GEN = ue_wrap::generator;
using coop::net::PowerGridPayload;
using coop::net::kPowerGridGenerators;

bool ReadRows(PowerGridPayload& p) {
    std::vector<void*> gens;
    if (GEN::ReadGenerators(gens) == 0) return false;
    const size_t n = std::min(gens.size(), static_cast<size_t>(kPowerGridGenerators));
    p.op = coop::net::kPowerGridOpRows;
    p.count = static_cast<uint8_t>(n);
    for (size_t i = 0; i < n; ++i) {
        GEN::Row r{};
        coop::net::PowerGridRow& w = p.rows[i];
        w = {};
        if (!gens[i] || !GEN::ReadRow(gens[i], r)) continue;
        w.present = 1;
        w.broken = r.broken ? 1 : 0;
        w.cyc = r.cyc ? 1 : 0;
        w.upgradeLevel = static_cast<uint8_t>(std::clamp(r.upgradeLevel, 0, kMaxUpgrade));
        w.cycle = r.cycle;
        coop::power_puzzle::Fill(gens[i], w.puzzle);
    }
    return true;
}

bool SameRows(const PowerGridPayload& a, const PowerGridPayload& b) {
    if (a.count != b.count) return false;
    for (int i = 0; i < a.count && i < kPowerGridGenerators; ++i) {
        const auto& x = a.rows[i];
        const auto& y = b.rows[i];
        if (x.present != y.present || x.broken != y.broken || x.cyc != y.cyc || x.upgradeLevel != y.upgradeLevel ||
            x.cycle != y.cycle || !coop::power_puzzle::SamePuzzle(x.puzzle, y.puzzle))
            return false;
    }
    return true;
}

}  // namespace coop::power_grid::row_reading
