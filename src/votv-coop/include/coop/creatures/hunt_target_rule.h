// coop/creatures/hunt_target_rule.h -- the choice of the player a host's creature hunts, as a pure function.
//
// A creature's Blueprint reads "the player" as player 0, the host. The host's creatures that hunt (coop/creatures/hunt_target
// lists them) and the walking mannequin (coop/creatures/walker_target) answer those reads with the body of the player this rule
// picks, within the creature's OWN range:
//   KEEP     a creature hunts one player until it loses it, so a nearer player does not take it over.
//   LOSE     the target is not alive, or lies beyond the creature's own range (a range of 0 is a creature whose Blueprint has
//            none), or has no body to measure: the host alone is kept without a measure, since its body is the Blueprint's own.
//   ACQUIRE  with no target, the nearest living player within the range, the earlier slot on a tie; with none in range, the
//            host, so the Blueprint's own branch for "the player is not near" runs as it does for the host alone.
// The dev switch `hunt_skip_host` (VOTVCOOP_HUNT_SKIP_HOST) takes the host out of the candidates and out of the keep, so with
// every player at one spot a hunter must pick a client; the host stays the fallback. The rule is the one the mannequin's rule
// was (coop/creatures/walker_target_rule.h cites its precedents: MTA's syncer is kept while it stays near, Source's enemy is
// re-chosen on sight of a nearer one, and this keeps like the first); that file now calls this one with the mannequin's range.
// No engine, no session: callable from any thread.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::hunt_target {

// The slot of a creature that has no target, and the host's slot (the player a creature falls back to).
inline constexpr uint8_t kNoTarget = 0xFF;
inline constexpr uint8_t kHostSlot = 0;

// One player as the creature's read finds it: the slot, whether it counts as alive (the host always does), and its 3D distance
// from the creature, negative when its body did not read.
struct PlayerView {
    uint8_t slot;
    bool    alive;
    float   distanceCm;
};

// The creature's own range, and the dev switch. `rangeCm` is the farthest a player may stand for the creature to go for it (a
// player AT the range is in, as a Blueprint's "farther than" test says); 0 or less means the Blueprint has none.
struct Rule {
    float rangeCm;
    bool  skipHost;
};

// The slot the creature hunts after this read: `current` (kNoTarget for none) kept or replaced as above. `players` lists each slot
// at most once, in slot order. Pure.
uint8_t Decide(uint8_t current, const PlayerView* players, size_t n, Rule rule);

// The rule's boot selftest: each row from a stated input. Prints `hunt_target selftest: ALL PASS (N checks)` or, per failing
// row, `hunt_target selftest FAIL: <what>`. True when every row passed.
bool RunSelftest();

}  // namespace coop::hunt_target
