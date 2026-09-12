// ue_wrap/core/uobject_listeners.h -- the engine's own report of every UObject birth and death.
//
// FUObjectArray keeps two listener lists. AllocateUObjectIndex calls every create listener on the
// thread constructing the object, the instant its slot is taken; ConditionalFinishDestroy calls
// every delete listener under the array's lock, at purge time, before the slot is freed. This
// module registers one of each, appended to the engine's arrays the way the engine's own
// registrars append (no engine function is called), and turns both notifications into a queue
// the game thread drains. A callback runs inside the engine with the object half-built (create)
// or half-destroyed (delete), so it records the pointer, the class and the slot index and nothing
// else; every read that needs the object waits for the drain.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ue_wrap::uobject_listeners {

struct Event {
    void*   obj;      // the UObject; valid at the drain only if the slot at `index` still holds it
    void*   cls;      // its class, read at a birth; null for a death (the slot names it)
    int32_t index;    // its GUObjectArray slot
    bool    created;  // false = deleted
};

// Register with the engine and start recording. Idempotent; false until reflection has resolved
// the array and the engine heap. Any thread: the delete list is appended under the engine's own
// lock, the create list with the store order its lock-free reader tolerates.
bool Install();

// Stop recording and drop what is queued. The listener objects stay registered until the engine's
// own shutdown, where each removes itself; there is no re-install.
void Uninstall();

// Move every event recorded since the last call to the back of `out`, in the order the engine
// reported them. Returns the number moved. Game thread.
size_t Drain(std::vector<Event>& out);

struct Stats {
    uint64_t created;    // create notifies recorded since Install
    uint64_t deleted;    // delete notifies recorded since Install
    size_t   highWater;  // the deepest the queue got between two drains
};
Stats GetStats();

}  // namespace ue_wrap::uobject_listeners
