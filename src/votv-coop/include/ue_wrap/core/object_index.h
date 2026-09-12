// ue_wrap/core/object_index.h -- which objects of each class exist, kept current by the engine's
// own create and delete notifications rather than by walking the object array.
//
// The index is seeded once from the live array on the game thread and then maintained by the
// events uobject_listeners drains: a birth links the slot into its class's list, a death unlinks
// it. Every applied event is checked against the slot it names, so an object that died before its
// birth was drained, or a slot the engine recycled, never leaves a stale entry. Membership means
// "allocated in the array": a member may still be under construction, on the loading thread, or
// marked for death; the slot's flags (reflection::slot_flags) say which, and every reader checks
// them before touching the object. An unreachable object is never seeded and never handed out as
// a class's instance: only unreachable objects are freed, by a purge thread that runs beside the
// game thread, so a pointer to one is unsafe from the moment the bit is set.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ue_wrap::object_index {

// Register the listeners. Idempotent; false until reflection has resolved. The seed runs on the
// first Drain, so events recorded between the two are folded in rather than lost.
bool Install();

// Apply the queued events, seeding first; budgeted, so a level load's backlog spreads over ticks.
// Returns the number of events applied. Game thread.
size_t Drain();

bool IsSeeded();

using InstanceFn = void (*)(void* ctx, void* obj, int32_t index);
using ClassFn    = void (*)(void* ctx, void* cls, void* anyInstance);

// Every live instance of exactly `cls` (not its subclasses), in no particular order. Returns the
// count visited. Game thread; the callback must not create or destroy objects.
size_t ForEachInstance(void* cls, InstanceFn fn, void* ctx);

// Every class with at least one readable instance, with one of them (slot still held, not
// unreachable). Same contract.
size_t ForEachClass(ClassFn fn, void* ctx);

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
    size_t objects;   // entries in the index
    size_t classes;   // classes with at least one entry
};
Parity DebugCompareWithWalk();

}  // namespace ue_wrap::object_index
