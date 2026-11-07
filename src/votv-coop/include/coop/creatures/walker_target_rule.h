// coop/creatures/walker_target_rule.h -- the walking mannequin's choice of the player it hunts, as a pure function.
//
// The host's walker reads its player at its move proxy's end, in its out-of-range delay loop and at BeginPlay (coop/creatures/
// walker_target answers those reads with its target's body). The rule that picks the target there needs no engine: given who
// the walker hunts now and how each player stands to it, it answers who it hunts next.
//   KEEP     a walker hunts one player until it loses it, so a nearer player does not take it over.
//   LOSE     the target is not alive, or lies beyond the Blueprint's own range (the same test its range branch makes at the same
//            read), or has no body to measure: the host alone is kept without a measure, since its camera is the Blueprint's own.
//   ACQUIRE  with no target, the nearest living player within the range, the earlier slot on a tie; with none in range, the
//            host, so the Blueprint's own out-of-range branch waits and asks again.
// The module builds one PlayerView per slot at the walker's read and applies the answer; the boot selftest runs this rule on stated
// inputs, since no honest run reads the players while one stands nearer (the Blueprint reads only where its target is lost or
// reached). No engine, no session: callable from any thread.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::walker_target {

// The Blueprint's own range: its hunt waits and asks again when the camera manager is farther than this.
inline constexpr float kRangeCm = 20000.f;

// The slot of a walker that has no target, and the host's slot (the player a walker falls back to).
inline constexpr uint8_t kNoTarget = 0xFF;
inline constexpr uint8_t kHostSlot = 0;

// One player as the walker's read finds it: the slot, whether it counts as alive (the host always does), and its 3D distance
// from the walker, negative when its body did not read.
struct PlayerView {
    uint8_t slot;
    bool    alive;
    float   distanceCm;
};

// The slot the walker hunts after this read: `current` (kNoTarget for none) kept or replaced as above. `players` lists each
// slot at most once, in slot order. Pure.
uint8_t DecideTarget(uint8_t current, const PlayerView* players, size_t n);

// The rule's boot selftest: each row from a stated input. Prints `walker_target selftest: ALL PASS (N checks)` or, per failing
// row, `walker_target selftest FAIL: <what>`. True when every row passed.
bool RunSelftest();

}  // namespace coop::walker_target
