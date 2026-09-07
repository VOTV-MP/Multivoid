// coop/element/element_deleter.h -- the deferred Element destruction queue.
//
// Adapted from `reference/mtasa-blue/Client/mods/deathmatch/logic/CElementDeleter.{h,cpp}` (MIT):
// MTA never destroys a CClientEntity synchronously at the call site. Delete() flags it
// m_bBeingDeleted, unlinks it from its manager and ID array, and appends it to a pending list; the
// real `delete` happens once per frame in DoDeleteAll(). Ours is the same shape, simplified -- no
// Lua SmartPointer carve-out, no recursive child delete, no per-VM cleanup -- just park the
// unique_ptr and destroy it later at one controlled game-thread point.
//
// WHY DEFER, the root reason this exists for us: our ProcessEvent observer and interceptor
// callbacks can dispatch on parallel-anim task-graph WORKER threads and not only on the game
// thread. So an Element's owner map -- npc_sync's g_npcElements, a MirrorManager<T> -- may drop its
// unique_ptr, and therefore run ~Element and Registry::FreeId, on a worker the instant a
// K2_DestroyActor PRE fires, racing any in-flight raw pointer to that Element. Routing the drained
// unique_ptr through here moves the free to one game-thread Flush point.

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

    // Park `element` for deferred destruction. Flags it being-deleted immediately, so a concurrent
    // IsBeingDeleted() gate treats it as dead from this point, and moves the unique_ptr into the
    // pending queue under the internal mutex. No-op on null. Accepts any Element subclass through
    // the implicit unique_ptr<T> to unique_ptr<Element> upcast, and Element's destructor is
    // virtual, so destroying through the base pointer is correct.
    //
    // Callable from any thread -- the owner-map drain thread, possibly a worker -- and it takes the
    // internal mutex only across the queue push.
    //
    // Deferring is a STRICT improvement over destructing immediately after the Take; it does not by
    // itself make a read that races the Flush safe. The IsBeingDeleted() gate at each resolve site
    // closes that, and the flag is set here so the gate sees a parked Element as already dead the
    // moment it is queued.
    void Enqueue(std::unique_ptr<Element> element);

    // Destroy everything queued since the last call, returning the count flushed. MUST run on the
    // game thread. It swaps the queue out under the internal mutex and then lets the unique_ptrs
    // destruct OUTSIDE it, because each ~Element takes the Registry mutex through FreeId and
    // UnregisterMirror: the deleter mutex has to stay a LEAF and is never held across a destructor.
    // Cheap when empty -- one uncontended mutex acquire and a check -- which is the steady state,
    // since this is the per-tick call in net_pump.
    //
    // That tick is the ONLY Flush call site, and there is deliberately no process-exit Flush: on
    // shutdown the net-pump loop stops before a final tick, so an element still queued is
    // destructed at static-singleton teardown. That is safe, because ~Registry's
    // NotifyRegistryShuttingDown latch makes ~Element skip FreeId once the Registry is gone, but it
    // is not the controlled game-thread point, so do not rely on a queued element's FreeId running
    // at exit.
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
