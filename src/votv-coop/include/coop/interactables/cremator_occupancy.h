// coop/interactables/cremator_occupancy.h -- who enters which cremator's burn volume.
//
// The cremator's own `burning` component begins an overlap when an actor steps into the burn volume: the Blueprint's
// handler collects the actor into the machine's list. This module watches that begin-overlap after its body ran and
// hands each one to a short list of listeners, and answers the standing question for an actor already inside. It
// judges nothing: the verbs that care (the crematorium order, the cremator probe and drill) take the event on their
// own tick. Game thread throughout.

#pragma once

namespace coop::cremator_occupancy {

// A begin-overlap of a cremator's burn volume, seen after its body ran. `cremator` is the machine that handled it,
// `other` the overlapping actor. A listener only records.
using OverlapFn = void (*)(void* cremator, void* other);

// Adds a listener once (the same function again adds nothing) and makes sure the watch on the cremator's begin-overlap
// is registered. False while the list is full or the script gate refuses the registration, so a caller may retry on
// its next tick; true once the listener is in and the watch is registered.
bool AddListener(OverlapFn fn);

// Whether the watch on the cremator's begin-overlap is live: registered and its names resolved, so the script gate
// really will deliver. AddListener answers true the moment it registers; a verb that must be able to SEE a landing asks
// this before it moves anybody.
bool WatchLive();

// Whether the cremator's burn volume overlaps `actor` now (the volume's own IsOverlappingActor). False for a dead
// cremator, a volume that did not resolve, or a dead actor.
bool IsOverlapping(void* cremator, void* actor);

}  // namespace coop::cremator_occupancy
