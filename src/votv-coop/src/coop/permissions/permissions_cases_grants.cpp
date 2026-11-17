// coop/permissions/permissions_cases_grants.cpp -- the checks of coop/permissions/grants_core.h: the
// bit's read rule across serials and roles, the recompute rule at its edges, the expiry merge, and
// the table's defaults.

#include "coop/permissions/grants_core.h"
#include "coop/permissions/permissions_selftest.h"

#include <cstddef>
#include <cstdint>

namespace coop::permissions {

void RunGrantsCases(CheckSink& sink) {
    namespace G = grants;
    constexpr uint32_t kSerial = 3;
    constexpr uint32_t kHudOnly = 1u << static_cast<uint32_t>(G::Projected::Hud);
    const uint64_t word = G::Pack(kSerial, kHudOnly);

    sink.Check(G::ReadBit(word, kSerial, false, G::Projected::Hud), "grants: an equal serial reads a set bit");
    sink.Check(!G::ReadBit(word, kSerial, false, G::Projected::Freecam),
               "grants: an equal serial reads a clear bit");
    sink.Check(G::ReadBit(word, kSerial + 1, true, G::Projected::Freecam),
               "grants: another serial reads true on the host");
    sink.Check(!G::ReadBit(word, kSerial + 1, false, G::Projected::Hud),
               "grants: another serial reads false on a client");
    sink.Check(!G::ReadBit(G::Pack(0, 0xFu), 1, false, G::Projected::Hud),
               "grants: serial 0 with every bit set reads false on a client");

    constexpr int64_t kEarliest = 500;
    const G::DueState last{7, kSerial, kEarliest};
    sink.Check(G::Due(last, 8, kSerial, 100), "grants: a changed revision is due");
    sink.Check(G::Due(last, 7, kSerial + 1, 100), "grants: a changed serial is due");
    sink.Check(G::Due(last, 7, kSerial, kEarliest + 1), "grants: past the earliest expiry is due");
    sink.Check(!G::Due(last, 7, kSerial, kEarliest), "grants: at the earliest expiry is not yet due");
    sink.Check(!G::Due(G::DueState{7, kSerial, 0}, 7, kSerial, 1'000'000),
               "grants: no expiry and nothing changed is never due");

    sink.Check(G::MergeEarliest(0, 5) == 5, "grants: merging none with 5 is 5");
    sink.Check(G::MergeEarliest(7, 5) == 5, "grants: merging 7 with 5 is 5");
    sink.Check(G::MergeEarliest(5, 0) == 5, "grants: merging 5 with none is 5");

    bool defaultsRight = true;
    for (size_t i = 0; i < static_cast<size_t>(G::Projected::Count); ++i)
        if (G::kProjected[i].defaultGranted != (i == static_cast<size_t>(G::Projected::Cheats))) defaultsRight = false;
    sink.Check(defaultsRight, "grants: every projected node is denied by default but the cheats right");
}

}  // namespace coop::permissions
