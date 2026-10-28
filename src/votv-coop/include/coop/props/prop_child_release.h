// coop/props/prop_child_release.h -- a jointed child waits for its parent. A dragged door (a lid, a hatch) is
// jointed to the actor it hangs from, and a free, simulating child on a kinematic parent that is still moving is
// dragged by it. When the door's release arrives while the driven-prop stream still has the parent parked and
// interpolating, the door stays parked at its release pose and is let go the moment the parent is: its End
// applied, or a hand taking it. A parent that never ends releases the door after kChildWaitMs. Game thread,
// every function.

#pragma once

#include <cstdint>

namespace coop::prop_child_release {

// A failure bound, never a wait the design relies on: the parent's End is the release.
constexpr uint64_t kChildWaitMs = 10000;

// `door` (already parked, at its release pose) is let go after `parent` is. The pose is read from the door now.
// A door already waiting takes the new parent and pose.
void Defer(void* door, void* parent);

// The door is released with its parent not parked: any wait it still had is stale and ends.
void Forget(void* door);

// The driven-prop stream gave `parent` up: every door waiting on it is placed at its recorded pose and unparked,
// after the parent. A door a hand holds meanwhile is not touched; its wait just ends.
void OnParentReleased(void* parent);

// Per tick: a wait whose door or parent died ends, and one past kChildWaitMs releases its door (WARN once).
// Nothing to do, and nothing allocated, while no door waits.
void Tick(uint64_t nowMs);

// The session ended: every door still waiting is unparked simulating, and the waits are dropped.
void OnDisconnect();

}  // namespace coop::prop_child_release
