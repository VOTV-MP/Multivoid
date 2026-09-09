// coop/interactables/atv_corrector.h -- how a MIRRORED ATV is driven toward its authority.
//
// ONE CONCEPT: what happens to a non-authored ATV when a state packet arrives, and nothing
// else. The sender-side change gate, the collision guard, the identity mint and the scan pass
// all stay in atv_sync.cpp. There is no per-frame mirror work at all -- the corrector runs
// ONLY on arrival, from the state-packet handler.
//
// The MTA precedent and the one deliberate divergence from it are documented at
// ApplyCorrection itself.

#pragma once

#include "coop/interactables/atv_sync_internal.h"

// Forward-declared rather than including protocol.h: this header needs only the NAME for a
// by-const-ref parameter, and protocol.h is the tree's largest constants header.
namespace coop::net { struct AtvStatePayload; }

#include <cstdint>

namespace coop::atv_corrector {

// Bias a mirrored ATV's velocity toward the authority's pose, or cut to that pose when the error
// is too far gone to close gracefully. `snap` forces the cut (a joiner's first packet).
// Game thread.
void ApplyCorrection(coop::atv_sync::AtvEntry& e, const coop::net::AtvStatePayload& p, bool snap);

// Session totals, for printing. A corrector nobody can see is a corrector nobody can falsify,
// so these exist to reach a log line -- atv_sync's teardown prints them at disconnect, and the
// ATV probe samples them while a session runs.
// Game thread.
struct Counters {
    uint64_t corrections = 0;
    uint64_t warps       = 0;
    uint64_t stallWarps  = 0;
    unsigned long long restPlaces = 0;  // re-places of a parked author's pose
};
Counters ReadCounters();

}  // namespace coop::atv_corrector
