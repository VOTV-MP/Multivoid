// coop/interactables/verb_lanes.h -- the interactable lanes that stand on a device's own verbs at the
// script-body gate, wired in one place: the drone console's press, a door's press, hit and pry and
// its open state, a light group's runTrigger, a toggle device's verb, a keypad's verbs. Each lane
// registers its watches (Install, a per-tick retry), settles them and runs its host queue (Tick),
// drops a leaver's queue (OnPeerLeft) and says its session's summary (OnDisconnect); this calls them,
// in that order. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::verb_lanes {

void Install(coop::net::Session& session);
void Tick(coop::net::Session& session);
void OnPeerLeft(uint8_t slot);
void OnDisconnect();

}  // namespace coop::verb_lanes
