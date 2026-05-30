// coop/element/element_deleter.h -- deferred Element destruction queue.
//
// Adapted from `reference/mtasa-blue/Client/mods/deathmatch/logic/
// CElementDeleter.{h,cpp}` (MIT): MTA never destroys a CClientEntity
// synchronously at the call site. Delete() flags it m_bBeingDeleted, unlinks
// it from its manager/ID array, and appends it to a pending list; the real
// `delete` happens once per frame in DoDeleteAll() (CClientGame.cpp:1309).
// Our version is the same shape, simplified: no Lua SmartPointer carve-out,
// no recursive child delete, no per-VM cleanup -- just "park the unique_ptr,
// destroy it later at one controlled game-thread point".
//
// WHY DEFER (the root reason this exists for us):
//   Our ProcessEvent observer / interceptor callbacks can dispatch on
//   parallel-anim task-graph WORKER threads, not only the game thread
//   (element.h:30-44; ue_wrap/game_thread.h:118-120). So an Element's owner
//   map (npc_sync's g_npcElements, a MirrorManager<T>, ...) may drop its
//   unique_ptr -- and thus run ~Element -> Registry::FreeId -- on a worker,
//   the instant a K2_DestroyActor PRE fires. Destroying the C++ Element at
//   that arbitrary worker instant races any in-flight raw pointer to it.
//   Routing the drained unique_ptr through here moves the actual free to a
//   single game-thread Flush point (top of net_pump::Tick), narrowing the
//   window from "any observer instant on any thread" to "one controlled
//   point". This is a STRICT improvement over immediate destruct-after-Take;
//   it does NOT by itself make a read that races the Flush safe -- the
//   IsBeingDeleted() gate at each resolve site (ScopedElementRef, a later
//   increment) closes that, and Enqueue sets the flag here so the gate sees a
//   parked Element as already-dead the moment it is queued.
//
// THREAD-SAFETY / LOCK ORDER:
//   - Enqueue() is callable from any thread (the owner-map drain thread,
//     possibly a worker). It takes the internal mutex only across the queue
//     push.
//   - Flush() MUST run on the game thread. It swaps the queue out under the
//     internal mutex, then releases the unique_ptrs OUTSIDE the mutex. Each
//     ~Element takes the Registry mutex (FreeId / UnregisterMirror), so the
//     deleter mutex must stay a LEAF -- never held across a dtor. Same
//     drain-then-destruct discipline as MirrorManager<T> (mirror_manager.h).

#pragma once

#include "coop/element/element.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace coop::element {

class ElementDeleter {
public:
    // Process-lifetime singleton (Meyers; thread-safe init since C++11).
    static ElementDeleter& Get();

    ElementDeleter(const ElementDeleter&)            = delete;
    ElementDeleter& operator=(const ElementDeleter&) = delete;

    // Park `element` for deferred destruction. Flags it being-deleted
    // immediately (so a concurrent IsBeingDeleted() gate treats it as dead
    // from this point) and moves the unique_ptr into the pending queue under
    // the internal mutex. No-op on null. Accepts any Element subclass via the
    // implicit unique_ptr<T> -> unique_ptr<Element> upcast (Element's dtor is
    // virtual, so destruction through the base pointer is correct).
    void Enqueue(std::unique_ptr<Element> element);

    // Destroy everything queued since the last call. MUST run on the game
    // thread. Swaps the queue out under the mutex, then lets the unique_ptrs
    // destruct OUTSIDE the mutex (~Element -> Registry::FreeId /
    // UnregisterMirror take the Registry mutex). Returns the count flushed.
    // Cheap when empty (a single uncontended mutex acquire + empty check),
    // which is the steady state -- this is the per-tick call in net_pump.
    //
    // The ONLY Flush call site is net_pump::Tick. There is deliberately NO
    // process-exit Flush: on shutdown the net-pump loop stops before a final
    // tick, so any element still queued is destructed at static-singleton
    // teardown. That is safe -- ~Registry's NotifyRegistryShuttingDown latch
    // makes ~Element skip FreeId once the Registry is gone -- but it is NOT the
    // controlled game-thread point, so do not rely on a queued element's FreeId
    // actually running at exit. (A handful of host Npc Elements parked in the
    // last ~8ms before X-close fall in this bucket; harmless.)
    size_t Flush();

    // Pending count (diagnostics / self-test). Takes the mutex.
    size_t PendingCount() const;

private:
    ElementDeleter()  = default;
    ~ElementDeleter() = default;

    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<Element>> m_pending;
};

}  // namespace coop::element
