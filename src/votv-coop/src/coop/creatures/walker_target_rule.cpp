// coop/creatures/walker_target_rule.cpp -- see coop/creatures/walker_target_rule.h.

#include "coop/creatures/walker_target_rule.h"

#include "coop/player/players_registry.h"

namespace coop::walker_target {

static_assert(kHostSlot == coop::hunt_target::kHostSlot && kNoTarget == coop::hunt_target::kNoTarget,
              "the mannequin's slots are the general rule's");
static_assert(kHostSlot == coop::players::kPeerIdHost, "the rule's host slot is the registry's");

uint8_t DecideTarget(uint8_t current, const PlayerView* players, size_t n) {
    return coop::hunt_target::Decide(current, players, n, coop::hunt_target::Rule{kRangeCm, /*skipHost=*/false});
}

}  // namespace coop::walker_target
