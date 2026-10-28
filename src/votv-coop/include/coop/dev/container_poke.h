// coop/dev/container_poke.h -- [dev] the organic container mutation the container drills share: the game's own
// extract(0) on a world container, so the addObject/takeObj watch sees what a player's extraction would send.

#pragma once

#include <cstdint>

namespace coop::dev::container_poke {

// Dispatch `Aprop_container_C::extract(0)` on the actor of world container `eid`. `before` and `after` are the
// container's record counts around the call (-1 when unreadable). False when the dispatch function does not
// resolve for this actor's class, with nothing fired. It spawns the extracted prop into the world. Game thread.
bool Extract(void* actor, int32_t& before, int32_t& after, uint32_t eid);

}  // namespace coop::dev::container_poke
