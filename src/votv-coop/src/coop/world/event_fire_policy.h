// coop/world/event_fire_policy.h -- the replay policy's one entry, private to event_fire_sync (a
// co-located src header, not include/): the rows themselves are event_fire_policy.cpp.
#pragma once

#include <string>

namespace coop::event_fire_sync {

// 1 replay, 0 known no-replay (laneOut names who carries the outputs, or why nobody does), -1
// unknown -- the default is no replay.
int ReplayVerdict(const std::string& name, const char** laneOut);

}  // namespace coop::event_fire_sync
