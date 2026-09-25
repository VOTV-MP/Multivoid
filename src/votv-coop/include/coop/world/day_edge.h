// coop/world/day_edge.h -- a client's own share of the host's midnight. A client's clock never rolls a
// midnight of its own (coop/world/time_sync holds it), so the rollover's outputs its machine keeps for its
// own player come here, at the host's day edge: the sample that moves the client's day number forward.
// The profile's days lived and the cycle's count of midnights since its world loaded (sleeplessDays)
// move by the days crossed, the day's music flags
// are set again, and the achievements the rollover progresses on a day number progress on this machine's
// profile (principle 6: a profile belongs to its player's machine). The world's outputs -- the hash codes,
// the task, the mail and points, the Bad Sun and the spawns -- stay the host's. The game's own order and
// gates, read from its bytecode: days_total in the roll's first pop, then the music flags, the day-number
// achievements (game mode 0 only, each tested on its own: day 30 and on, and the story's last event day)
// and the midnights since load, the insomniac achievement at seven.

#pragma once

#include <cstdint>

namespace coop::day_edge {

// CLIENT: the host's sample moved this client's day number from `from` to `to` (to > from), in the world
// of `cycle` and its save slot `saveSlot`. Game thread.
void OnHostDayEdge(void* cycle, void* saveSlot, int32_t from, int32_t to);

}  // namespace coop::day_edge
