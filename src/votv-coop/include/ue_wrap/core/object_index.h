// ue_wrap/core/object_index.h -- which objects of each class exist, kept current by the engine's
// own create and delete notifications rather than by walking the object array.
//
// Seeded once from the live array on the game thread, then maintained by the events
// uobject_listeners drains: a birth links the slot into its class's list, a death unlinks it. The
// drain runs a tick or more behind the engine, which meanwhile hands a dead object's slot, and often
// its address, to its next allocation, so a pointer cannot tell the object an event named from a new
// tenant: a birth is linked, and an entry handed out, only while the slot holds the pointer, the
// object is not unreachable and its class is still the listed one. Membership means "allocated in
// the array": a member may be under construction, on the loading thread, or marked for death; the
// slot's flags (reflection::slot_flags) say which, and every reader checks them. An unreachable
// object is unsafe the moment its bit is set, since a purge thread beside the game thread frees
// exactly those. The notifications are the engine's own; uobject_listeners.h names where UE4SS
// registers on the same lists.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ue_wrap::object_index {

// Register the listeners, and the drain as the game-thread dispatcher's prologue: before every batch
// of posted tasks, from the first after boot, the index applies the engine's notifications up to its
// per-drain budget, so after a level load it trails for a few batches. Idempotent; false until
// reflection has resolved. The seed
// runs on the first Drain, so events recorded between the two are folded in rather than lost.
bool Install();

// Apply the queued events, seeding first; budgeted per call, and the dispatcher's prologue calls it
// once per batch of posted tasks, so a level load's backlog spreads over batches (a frame with several
// batches applies several budgets). Returns the number of events applied. Game thread; re-entry from
// inside a drain applies nothing.
size_t Drain();

bool IsSeeded();

using InstanceFn = void (*)(void* ctx, void* obj, int32_t index);
using ClassFn    = void (*)(void* ctx, void* cls, void* anyInstance);

// Every live instance of exactly `cls` (not its subclasses), in no particular order: each entry's
// slot still holds it, it is not unreachable, and its class is `cls`. Returns the count handed out.
// Game thread; the callback must not create or destroy objects.
size_t ForEachInstance(void* cls, InstanceFn fn, void* ctx);

// Every class with at least one such instance, with one of them. Same contract.
size_t ForEachClass(ClassFn fn, void* ctx);

// The loaded class whose short name is `name`, compared without case as the engine compares names:
// a class object the index holds, found in one lookup whether or not the class has an instance.
// Null when no such class is loaded. Where two packages load classes of one name, the first still
// standing. Game thread.
void* ClassByName(const wchar_t* name);

// The one observer of the class set and of births (the scan hub): a class's first instance
// appeared, its last one went, an instance was born (after the class callback, when both fire).
// Called from Drain; seeded objects appear only through OnClassAppeared.
struct Observer {
    void* ctx;
    void (*OnClassAppeared)(void* ctx, void* cls, void* firstObj);
    void (*OnClassGone)(void* ctx, void* cls);
    void (*OnObjectCreated)(void* ctx, void* obj, void* cls, int32_t index);
};
void SetObserver(const Observer& o);

// The parity instrument: the live array against the index, inside one game-thread call. Live
// objects the index lacks are re-checked after a second drain, so a birth in flight during the
// walk does not count; entries the array no longer confirms count as they are.
struct Parity {
    size_t liveNotIndexed;
    size_t indexedNotLive;
    size_t misclassed;   // entries whose live object is of another class than the one listed
    size_t objects;      // entries in the index
    size_t classes;      // classes with at least one entry
};
Parity DebugCompareWithWalk();

// Test hooks: coop/dev/recycled_slot_drill stages a recycled slot through them. Game thread.
// Unlink whatever the slot lists; nothing when it lists nothing.
void DebugUnlinkSlot(int32_t index);
// Apply a birth as a drain applies one, through the same checks.
void DebugApplyCreate(void* obj, void* cls, int32_t index);
// What the slot lists, read raw with no hand-out check; false when it lists nothing.
bool DebugSlotListing(int32_t index, void** obj, void** cls);
// List `obj` under `cls` with no check at all: the state a queued death leaves when the slot's next
// tenant took the same address before the drain.
void DebugListUnchecked(void* obj, void* cls, int32_t index);

}  // namespace ue_wrap::object_index
