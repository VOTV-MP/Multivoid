// ue_wrap/world/active_events.h -- the gamemode's running-event refcount and the events holding it.
//
// lib_C::setEvent adds 1 to mainGamemode.activeEvents and appends its caller to
// activeEvents_senders when an event becomes active, and reverses both when it ends, clamping a
// negative count back to 0; the sender registers itself, so its class names the event. lib_C::getEvent
// -- what refuses sleep, wakes a sleeper and blocks a save -- is `activeEvents > 0` or the camera far
// outside the map. One reader for every consumer. Principle 7: no network or gameplay state; game
// thread only.

#pragma once

#include <cstdint>

namespace ue_wrap::active_events {

// Resolve the two members on mainGamemode_C. Retried on a 2 s throttle while the class loads; after
// five passes that had the class and still came up short it latches OFF with one warning, since a
// member missing from a loaded class does not appear later. True once both are in hand.
bool EnsureResolved();

// activeEvents, the refcount. False while unresolved or without a gamemode.
bool ReadCount(int32_t& out);

// activeEvents_senders: the engine's own array, valid only for the current game-thread slice.
struct Senders {
    void* const* data;
    int32_t      num;
};
bool ReadSenders(Senders& out);

}  // namespace ue_wrap::active_events
