// coop/creatures/hunt_target_rule.cpp -- see coop/creatures/hunt_target_rule.h.

#include "coop/creatures/hunt_target_rule.h"

#include "coop/player/players_registry.h"

namespace coop::hunt_target {
namespace {

static_assert(kHostSlot == coop::players::kPeerIdHost, "the rule's host slot is the registry's");

const PlayerView* ViewOf(const PlayerView* players, size_t n, uint8_t slot) {
    for (size_t i = 0; i < n; ++i)
        if (players[i].slot == slot) return &players[i];
    return nullptr;
}

bool InRange(float distanceCm, float rangeCm) { return rangeCm <= 0.f || distanceCm <= rangeCm; }

// Is `v` still the creature's target at this read. The host is kept without a measure (its distance is the Blueprint's own test and
// a read that cannot be measured is the Blueprint's to judge) unless the dev switch left it out; a client must be alive, have a body
// that read, and lie in range.
bool Keeps(const PlayerView* v, Rule rule) {
    if (!v || !v->alive) return false;
    if (v->slot == kHostSlot && rule.skipHost) return false;
    if (v->distanceCm < 0.f) return v->slot == kHostSlot;
    return InRange(v->distanceCm, rule.rangeCm);
}

// The nearest living player that read, within the range; the host when none is, so the Blueprint's own branch for a player that is
// not near runs.
uint8_t Acquire(const PlayerView* players, size_t n, Rule rule) {
    uint8_t best = kHostSlot;
    float   bestCm = 0.f;
    bool    found = false;
    for (size_t i = 0; i < n; ++i) {
        const PlayerView& v = players[i];
        if (!v.alive || v.distanceCm < 0.f) continue;
        if (v.slot == kHostSlot && rule.skipHost) continue;
        if (!InRange(v.distanceCm, rule.rangeCm) || (found && v.distanceCm >= bestCm)) continue;
        best = v.slot;
        bestCm = v.distanceCm;
        found = true;
    }
    return best;
}

}  // namespace

uint8_t Decide(uint8_t current, const PlayerView* players, size_t n, Rule rule) {
    if (current != kNoTarget && Keeps(ViewOf(players, n, current), rule)) return current;
    return Acquire(players, n, rule);
}

}  // namespace coop::hunt_target
