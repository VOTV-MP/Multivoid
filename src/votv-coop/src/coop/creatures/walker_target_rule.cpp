// coop/creatures/walker_target_rule.cpp -- see coop/creatures/walker_target_rule.h.

#include "coop/creatures/walker_target_rule.h"

#include "coop/player/players_registry.h"

namespace coop::walker_target {
namespace {

static_assert(kHostSlot == coop::players::kPeerIdHost, "the rule's host slot is the registry's");

const PlayerView* ViewOf(const PlayerView* players, size_t n, uint8_t slot) {
    for (size_t i = 0; i < n; ++i)
        if (players[i].slot == slot) return &players[i];
    return nullptr;
}

// Is `v` still the walker's target at this read. The host is kept without a measure (its distance is the Blueprint's own test and
// a read that cannot be measured is the Blueprint's to judge); a client must be alive, have a body that read, and lie in range.
bool Keeps(const PlayerView* v) {
    if (!v || !v->alive) return false;
    if (v->distanceCm < 0.f) return v->slot == kHostSlot;
    return v->distanceCm <= kRangeCm;
}

// The nearest living player that read, within the range; the host when none is, so the Blueprint's own out-of-range branch waits.
uint8_t Acquire(const PlayerView* players, size_t n) {
    uint8_t best = kHostSlot;
    float   bestCm = 0.f;
    bool    found = false;
    for (size_t i = 0; i < n; ++i) {
        const PlayerView& v = players[i];
        if (!v.alive || v.distanceCm < 0.f) continue;
        if (v.distanceCm > kRangeCm || (found && v.distanceCm >= bestCm)) continue;
        best = v.slot;
        bestCm = v.distanceCm;
        found = true;
    }
    return best;
}

}  // namespace

uint8_t DecideTarget(uint8_t current, const PlayerView* players, size_t n) {
    if (current != kNoTarget && Keeps(ViewOf(players, n, current))) return current;
    return Acquire(players, n);
}

}  // namespace coop::walker_target
