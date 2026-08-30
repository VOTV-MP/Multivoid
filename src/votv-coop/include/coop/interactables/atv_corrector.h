// coop/interactables/atv_corrector.h -- how a MIRRORED ATV is driven toward its authority.
//
// Extracted VERBATIM from atv_sync.cpp (2026-08-30): that file was 1095 LOC, past the 800-line
// soft cap, and the next work on this lane -- arc-1 commit 2's convergence-rate pass
// (docs/vehicles/ATV.md 15.2a) -- lands squarely inside this code. CLAUDE.md's extraction trigger
// says cut first and add second, so the new work arrives in its own file rather than growing the
// old one further.
//
// ONE CONCEPT: what happens to a non-authored ATV when a state packet arrives, and nothing else.
// The sender-side change gate, the collision guard, v77 identity and the scan pass all stay in
// atv_sync.cpp. There is no per-frame mirror work at all -- the corrector runs ONLY on arrival.
//
// Design of record: docs/vehicles/ATV.md 14 (as-built) and 15 (the first driven measurement, which
// is also where the convergence question now sits). The MTA precedent and the one deliberate
// divergence from it are documented at ApplyCorrection itself.

#pragma once

#include "coop/interactables/atv_sync_internal.h"
#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::atv_corrector {

// Bias a mirrored ATV's velocity toward the authority's pose, or cut to that pose when the error
// is too far gone to close gracefully. `snap` forces the cut (a joiner's first packet).
// Game thread.
void ApplyCorrection(coop::atv_sync::AtvEntry& e, const coop::net::AtvStatePayload& p, bool snap);

// Session totals, for printing. A corrector nobody can see is a corrector nobody can falsify
// (the 2026-08-29 lesson), so these exist to reach a log line -- atv_sync's teardown prints them.
// Game thread.
struct Counters {
    uint64_t corrections = 0;
    uint64_t warps       = 0;
    uint64_t stallWarps  = 0;
};
Counters ReadCounters();

}  // namespace coop::atv_corrector
