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

// The events the last drain took from the engine's notifications and left for the next, being over
// its budget: zero when, as of that drain, the index held every birth and death the engine had
// announced. After a level load it stays above zero for a few batches, and an object born in the
// load may be missing until then: the events apply in order, so at zero every one is in. Game thread.
size_t Backlog();

using InstanceFn = void (*)(void* ctx, void* obj, int32_t index);
using ClassFn    = void (*)(void* ctx, void* cls, void* anyInstance);

// Every live instance of exactly `cls` (not its subclasses), in no particular order: each entry's
// slot still holds it, it is not unreachable, and its class is `cls`. Returns the count handed out.
// Game thread; the callback must not create or destroy objects.
size_t ForEachInstance(void* cls, InstanceFn fn, void* ctx);

// Every class with at least one such instance, with one of them. Same contract.
size_t ForEachClass(ClassFn fn, void* ctx);

// A number that moves whenever a class gains its first listed instance or loses its last. ClassByName
// answers null while a class loads, and the class finishing moves no number, so a list derived from
// the class set and keyed on a looked-up class is rebuilt when the number moves or when the lookup's
// answer changes. Game thread.
uint64_t ClassSetVersion();

// Every class object the index has listed, in the order listed (a class loaded again is listed again):
// a log a consumer reads on from where it stopped, to meet every class once as it loads, with no walk.
// ClassLogAt answers the class at a position while it is listed (null once it is unlisted, or its slot
// holds another object), and says whether it has finished loading (its functions linked). The log only
// grows, a few bytes per class load of the process. Game thread.
size_t ClassLogSize();
void*  ClassLogAt(size_t position, bool* loaded);

// A number that moves whenever a class object is unlisted: a class unloaded, its functions and their
// scripts freed with it. A pointer to one of them read since the last move is still the object it was.
// Game thread.
uint64_t ClassUnlistVersion();

// The identity of `cls`'s current listing: 0 when the index does not list it, else a number no other listing of
// any class ever has, so a class unlisted and listed again, even at the same address, answers a new one. A holder
// of a pointer to a class, and of what it derived from the class, compares this with the value it stored: equal
// means that class was neither unloaded nor loaded again, whatever other classes did (ClassUnlistVersion moves
// for any). Never reads through `cls`, so a pointer to a class unloaded since is safe to ask. O(1), no
// allocation. Game thread (asserted), like the rest of the listing state.
uint64_t ClassListingId(void* cls);

// The loaded class whose short name is `name`, compared without case as the engine compares names:
// a class object the index holds, found in one lookup whether or not the class has an instance.
// Null when no such class is loaded, or while it is still being loaded. Where two packages load
// classes of one name, the first still standing. Game thread.
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

// Every birth and death of a listed object, at the drain that applies it, for a consumer that keeps its own
// index of a few classes as current as this one: after the observer's callbacks for a birth, before the
// unlink for a death, which carries the class the slot was listed under (a tenant replaced before its own
// death drained is reported dead too). Seeded objects are not reported, so a listener enumerates what it
// needs with ForEachInstance when it starts. A death's object may already be freed: compare its pointer,
// never read through it. A callback must not create or destroy objects. At most four, a second add of the
// same callbacks a no-op; false when full. Game thread. Source keeps entity listeners the same way
// (reference/source-sdk-2013/src/game/server/entitylist.h:343, IEntityListener).
struct Listener {
    void* ctx;
    void (*OnCreated)(void* ctx, void* obj, void* cls, int32_t index);
    void (*OnDeleted)(void* ctx, void* obj, void* cls, int32_t index);
};
bool AddListener(const Listener& l);

// Apply every event the engine has announced, past the per-drain budget: after it the index holds every birth
// and death up to this call, so a reader whose "not there" must be true at this instant asks for it. It lands
// a level load's backlog on one task, which the budget exists to spread, so it answers a miss, never a tick.
// Returns the events applied, or SIZE_MAX when it could not run: off the game thread, before Install, or
// inside a drain (an observer's or a listener's callback). Its calls, events and slowest call are in the
// 60 s summary.
size_t DrainAll();

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

// A boot check of the class listing identity (ClassListingId) on fake class objects and a fake name driven
// through the listing functions, so it needs no engine: listed, unlisted, listed again at the same address, a
// second class unlisting, and an address listed under two slots. Run before Install; it leaves no entry
// behind, and advances ClassUnlistVersion by 4 (no reader holds its absolute value). It runs on the boot thread
// and calls a game-thread accessor, silent there only because the game thread is not yet known: it must stay
// before Install.
// Logs "object_index selftest ALL PASS (<n> checks)" or one FAIL line per check.
bool RunSelftest();

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
