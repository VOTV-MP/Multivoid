// coop/dev/cremator_dev_support.h -- what the cremator probe's host arm and the cremator drill's burn arms share when
// they put a client in the firebox: the settled test of the client's slot. Private to coop/dev: the begin-overlap watch
// is coop/interactables/cremator_occupancy, and the machine's reads and calls are ue_wrap/devices/cremator. Game thread.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::cremator_support {

// Slot `slot`'s join has settled: its puppet stands here with a location, its world is ready, its join traffic is
// delivered and nothing is waiting in its reliable queue.
bool SlotSettled(coop::net::Session& session, uint8_t slot);

}  // namespace coop::dev::cremator_support
