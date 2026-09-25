// ue_wrap/world/event_list.h -- the story's event schedule, the list_events DataTable: one row an event,
// each with the game time it runs at. The rollover reads its last row's day as the story's end, where the
// alphaFinish achievement progresses. Engine-wrapper layer (principle 7): no network or gameplay logic.

#pragma once

#include <cstdint>

namespace ue_wrap::event_list {

// The day of list_events' last row (its time's Z), read once the table has loaded: a cooked asset that
// does not change. A table not loaded yet is asked for again at the next call; a row struct or member
// that does not resolve is latched, and each is said once. Game thread.
bool ReadLastDay(int32_t& out);

}  // namespace ue_wrap::event_list
