// coop/dev/cremator_dev_support.h -- what the cremator probe's host arm and the cremator drill's burn arms share when
// they put a client in the firebox: the cremator's begin-overlap watch, the settled test of the client's slot, and the
// move (the nearest firebox, its door opened, the admin verb's teleport order). Private to coop/dev: the machine's
// reads and calls are ue_wrap/devices/cremator. Game thread throughout.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::cremator_support {

// The cremator's own begin-overlap of its `burning` component, seen after its body ran: the event that collects the
// sitter. `cremator` is the machine that handled it, `other` the overlapping actor.
using OverlapFn = void (*)(void* cremator, void* other);

// Watches the begin-overlap and hands each one to `onOverlap` (a later call replaces the handler). False while the
// script gate refuses the registration, so a caller may retry on its next tick; true once it is registered.
bool WatchBurningOverlap(OverlapFn onOverlap);

// Slot `slot`'s join has settled: its puppet stands here with a location, its world is ready, its join traffic is
// delivered and nothing is waiting in its reliable queue.
bool SlotSettled(coop::net::Session& session, uint8_t slot);

// The cremator nearest slot `slot`'s puppet, with its firebox and its rotation.
struct Firebox {
    void*                cremator = nullptr;
    ue_wrap::FVector     at{};
    ue_wrap::FRotator    rot{};
    bool                 doorOpened = false;  // set by OpenAndSend
};

// Finds the cremator nearest the slot's puppet. False, `why` naming the step, when the puppet has no location, no
// cremator has a firebox, or the cremator's rotation does not read.
bool FindFirebox(uint8_t slot, Firebox& out, const char*& why);

// Opens the cremator's door, then orders slot `slot` to the firebox with the TeleportClient order, the pose the
// `/cremate` verb sends. False, `why` naming the step, when the door's open() did not run or the order was not sent.
bool OpenAndSend(coop::net::Session& session, uint8_t slot, Firebox& fb, const char*& why);

}  // namespace coop::dev::cremator_support
