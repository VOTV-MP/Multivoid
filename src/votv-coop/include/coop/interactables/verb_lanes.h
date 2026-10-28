// coop/interactables/verb_lanes.h -- the interactable lanes that stand on a device's own verbs at the
// script-body gate, wired in one place: the drone console's press, a door's press, hit and pry and
// its open state, a light group's runTrigger, a toggle device's verb, a security camera's use and
// laptop light button, a keypad's verbs, a press on the main desk's save family, a cord's plug and unplug, a press
// of a coffee device. Each lane registers its watches (Install, a per-tick retry), settles them and runs its host
// queue (Tick), and says its session's summary (OnDisconnect); a leaver's queues go with its slot's per-slot state. A
// lane that owes a joiner a seed sends it from ConnectReplayForSlot. This calls them, in that order. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::verb_lanes {

void Install(coop::net::Session& session);
void Tick(coop::net::Session& session);
// HOST, at a joiner's world-ready edge: the seeds the lanes here owe that slot.
void ConnectReplayForSlot(int slot);
void OnDisconnect();

}  // namespace coop::verb_lanes
