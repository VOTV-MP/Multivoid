// coop/dev/drill_chat_pace.h -- the spacing a drill keeps between a client's chat lines that must all be committed.
// The host judges a client's chat at Source's rate: a line passes 0.66 s after the slot last talked and while a two-tier
// bucket allows it, tier 1 refilling one line per 3 s (reference/source-sdk-2013/src/game/server/player.cpp:98-101,
// 8771-8818). A drill's burst would be refused past its first line, so its lines go one per this: tier 1's refill with
// a margin, which tier 2 carries for 41 lines. The rule's own number, not a wait for a milestone: a refused line has
// no answer a drill could wait on.

#pragma once

#include <cstdint>

namespace coop::dev {

inline constexpr uint64_t kTalkEveryMs = 3100;

}  // namespace coop::dev
