// coop/element/registry.h -- the unified ElementId allocator + O(1) resolver.
//
// Adapted from `reference/mtasa-blue/Client/mods/deathmatch/logic/CElementArray.{h,cpp}`
// (the client-side half) and `Server/.../CElementIDs.{h,cpp}` (the server-side
// half) (MIT). Single-class instead of MTA's split: we have one address space
// shared across host + clients, partitioned by range -- the role determines
// which range a peer may allocate from.
//
// Two-range partition (`element.h:kHostRangeSize`):
//   host range  [0, 32768)     -- host-only allocation. Analog of MTA server range.
//   peer range  [32768, 65536) -- client-local allocation. Analog of MTA client range.
//
// IDs are O(1) -> Element* via a fixed-size array. Allocation pops from a per-
// range free stack (LIFO) for cache-friendliness. Deletion immediately frees
// the id; subsequent allocations on the same side may reuse it. The Element's
// `m_syncContext` byte (bumped on each sync-relevant change) makes id reuse
// safe at the wire-protocol level.
//
// Thread safety: AllocHostId / AllocLocalId / FreeId / Get are mutex-guarded.
// The mutex is held only across the table+stack operations -- never during
// engine reflection or Element construction. Element subclass constructors
// call Registry on the constructing thread; the lock is held for microseconds.

#pragma once

#include "coop/element/element.h"

#include <cstddef>
#include <mutex>
#include <vector>

namespace coop::element { class Element; }

namespace coop::element {

class Registry {
public:
    // Singleton. Lazy-constructed on first call. Lives for the process
    // lifetime; OnDisconnect resets state but doesn't destroy the singleton.
    static Registry& Get();

    // Allocate a fresh ElementId in the host range, register `e` in the
    // lookup table, write the id back into the Element. Returns the id.
    // **Host role only** -- callers must guarantee `Session::role() == Host`.
    // Logs + returns kInvalidId if the host range is exhausted (32768 active
    // elements at once is well above expected peak; exhaustion = bug).
    ElementId AllocHostId(Element* e);

    // Allocate a fresh ElementId in the peer range for a client-local
    // element (something the local peer creates that does not need
    // authoritative routing). Same semantics as AllocHostId but on the
    // peer-range stack. Either role may call this.
    ElementId AllocLocalId(Element* e);

    // Return an id to its free stack and clear the table slot. Called from
    // Element destructor. No-op on kInvalidId. Logs + skips if the id is
    // already free (a double-free would indicate a lifetime bug).
    void FreeId(ElementId id);

    // Client-side mirror registration: bind an Element to a host-allocated
    // ElementId received over the wire. Used by client receivers (npc_sync
    // OnEntitySpawn, future Prop receivers etc.) to materialize a local
    // mirror of an entity the host owns.
    //
    // - Does NOT touch the free stacks. The id is in the host range; it
    //   was popped from the HOST's m_hostFree, not the client's, so the
    //   client must not return it to its own free stack on teardown.
    // - Sets `m_byId[id] = e` and stamps `Element::m_id = id` + `m_mirror = true`.
    // - Logs + returns false if the slot is already populated (duplicate
    //   spawn packet OR wire id collision -- both indicate a bug upstream).
    // - Logs + returns false if `id` is out of range or kInvalidId.
    //
    // The mirror's dtor calls UnregisterMirror via the `m_mirror=true` flag
    // (see ~Element). So the caller's only responsibility is to drop the
    // owning unique_ptr; the rest is automatic.
    bool RegisterMirror(ElementId id, Element* e);

    // Drop a client-side mirror. Clears m_byId[id]. Does NOT push to a free
    // stack (host range ids belong to the host's allocation space; pushing
    // would corrupt the client's stack with foreign ids over time).
    // No-op on kInvalidId / out of range / already empty.
    void UnregisterMirror(ElementId id);

    // O(1) lookup. Returns nullptr if `id` is kInvalidId, out of range, or
    // not currently allocated.
    Element* Get(ElementId id) const;

    // Quick range check. `kInvalidId` returns false on both.
    static bool IsHostId(ElementId id)  { return id < kHostRangeSize; }
    static bool IsLocalId(ElementId id) { return id >= kHostRangeSize && id < kMaxElements; }

    // Counts: number of currently-allocated elements per range. Used for
    // diagnostic logging + late-joiner snapshot sizing estimates.
    size_t HostCount() const;
    size_t LocalCount() const;

    // Snapshot-copy the (actor*, elementId) pair for every currently-
    // allocated Element of the given ElementType. The actor pointer is
    // extracted from Element::GetActor() under the internal mutex so
    // there's no use-after-free risk after the mutex releases (the actor
    // pointer is engine-owned, not Element-lifetime-bound).
    //
    // Used by the unified late-joiner snapshot path: prop_snapshot reads
    // actor*+eid pairs at TriggerForSlot time, then DrainChunk fills the
    // wire payload from each pair without any subsequent Registry mutex
    // acquisition.
    //
    // (Audit fix 2026-05-28: prior `vector<Element*>` API returned dangling
    // pointers if another thread freed an Element between the mutex
    // release and the caller's GetActor() call.)
    //
    // Returns count copied (out is cleared first).
    struct ActorIdPair { void* actor; ElementId id; };
    size_t SnapshotActorsByType(ElementType t, std::vector<ActorIdPair>& out) const;

    // INTENTIONALLY NO bulk-Reset() API (audited 2026-05-28). Each
    // subsystem owns the lifetime of the Elements it allocates and is
    // responsible for releasing them on its own OnDisconnect hook --
    // typically by draining its owner container so the Element destructors
    // fire and self-FreeId. A global Reset() would nuke other subsystems'
    // elements when one subsystem disconnects, corrupting their state +
    // double-freeing ids as their destructors later run. See
    // [[feedback-follow-mta-architecture-when-possible]] -- MTA's
    // CElementIDs has no bulk Reset for the same reason.

private:
    Registry();
    ~Registry();
    Registry(const Registry&)            = delete;
    Registry& operator=(const Registry&) = delete;

    mutable std::mutex m_mutex;
    Element* m_byId[kMaxElements] = {};   // index = ElementId; nullptr = free
    std::vector<ElementId> m_hostFree;    // LIFO free stack, host range
    std::vector<ElementId> m_localFree;   // LIFO free stack, peer range

    // Pre-populate the free stacks with the full id range (one-time at
    // construction + on Reset()). Called under m_mutex.
    void RefillFreeStacks_();
};

}  // namespace coop::element
