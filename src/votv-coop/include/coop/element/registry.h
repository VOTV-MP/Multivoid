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
#include <deque>
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
    //
    // D9-2 fix (2026-05-29 PR-FOUNDATION Tier 2): the peer range is
    // sub-partitioned into per-peer-slot bands so two CLIENT processes
    // never mint colliding ids (the host relay holds mirrors of BOTH and
    // RegisterMirror rejects a populated slot). Before SetLocalPeerBand
    // is called (boot/seed window, slot not yet known), this allocates
    // from the "pre-slot" band -- those ids are LOCAL-ONLY (a client's
    // seed-phase Aprop_C Init POST never broadcasts; gated by
    // !s->connected() + the Aprop_C client early-return), so cross-client
    // overlap of pre-slot ids is harmless. After SetLocalPeerBand, draws
    // from the client's exclusive slot band. See SetLocalPeerBand.
    ElementId AllocLocalId(Element* e);

    // Activate this CLIENT process's per-slot sub-band for AllocLocalId
    // (D9-2 fix). Called once from players::Registry::SetLocalPeerId when
    // the client learns its peer slot from AssignPeerSlot. `slot` must be
    // in [1, kMaxPeers); slot 0 (host) never calls this -- the host uses
    // AllocHostId exclusively for its own elements. Replaces m_localFree
    // with the slot band's ids. Pre-slot-band ids already handed out
    // remain valid in the lookup table and freeable (they were popped on
    // allocation, are not on the free stack, and FreeId returns them to
    // m_localFree where they recycle harmlessly). Idempotent on the same
    // slot. Thread-safe (acquires m_mutex). No-op + LOGW on slot 0 / out
    // of range.
    void SetLocalPeerBand(uint8_t slot);

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

    // ---- Wire-receiver range validation (PR-FOUNDATION-1, 2026-05-29) ----
    //
    // Sender-role-aware validation of an inbound elementId carried over the
    // wire. Closes the multi-site id-range trust gap surfaced by the
    // foundation audit (E-1 + D2-1 + D2-2 + the 3 PropSpawn/PropDestroy/Join
    // range gaps): a packet carrying an elementId in a range its sender
    // role isn't allowed to allocate from is forged or a relay-loop bug
    // and must be dropped at the boundary.
    //
    // - senderIsHost == true: accept only host-allocated eids,
    //                         i.e. [1, kHostRangeSize). Rejects 0,
    //                         kInvalidId, and any peer-range id.
    // - senderIsHost == false: accept only peer-allocated eids,
    //                          i.e. [kHostRangeSize, kMaxElements).
    //
    // Receivers determine `senderIsHost` from the GNS-stamped
    // `senderPeerSlot` (slot 0 == host). All receiver sites that ingest
    // an elementId from a peer MUST gate via this helper before passing
    // it to RegisterMirror / RegisterPropMirror / Install / etc.
    //
    // The two specialised flavors are exposed for sites where the sender
    // role is unconditional by feature (EntitySpawn / EntityDestroy are
    // host-only; client-sourced PropSpawn for chipPile/clump/trashBits is
    // client-only).
    static bool IsAllowedHostAllocatedEid(ElementId id) {
        return id != kInvalidId && id >= 1u && id < kHostRangeSize;
    }
    static bool IsAllowedPeerAllocatedEid(ElementId id) {
        return id != kInvalidId && id >= kHostRangeSize && id < kMaxElements;
    }
    static bool IsAllowedSenderEid(bool senderIsHost, ElementId id) {
        return senderIsHost ? IsAllowedHostAllocatedEid(id)
                            : IsAllowedPeerAllocatedEid(id);
    }

    // Counts: number of currently-allocated elements per range. Used for
    // diagnostic logging + late-joiner snapshot sizing estimates.
    size_t HostCount() const;
    size_t LocalCount() const;

    // Snapshot-copy the (actor*, elementId, internalIdx) tuple for every
    // currently-allocated Element of the given ElementType, under the internal
    // mutex. The mutex protects only the C++ Element lifetime (FreeId takes it
    // before clearing m_byId), so no Element* dangles after it releases.
    //
    // It does NOT protect the engine ACTOR pointer: that is a raw UObject* the
    // UE4 GC can free independently of any C++ mutex (a mass GC purge flags
    // ~2000 props PendingKill/Unreachable without firing per-actor
    // K2_DestroyActor, so their Prop Elements -- and the stale m_actor they
    // hold -- persist in the Registry). Consumers MUST therefore validate each
    // actor with reflection::IsLiveByIndex(actor, internalIdx) -- NOT IsLive,
    // whose first act is to deref the (possibly-purged) actor and AV. The
    // internalIdx is the Element's cached slot captured while the actor was
    // live. (Root-caused 2026-05-30: the connect-edge snapshot AV.)
    //
    // Used by the late-joiner snapshot path: prop_snapshot reads the tuples at
    // TriggerForSlot time, then DrainChunk fills the wire payload across ticks,
    // re-validating via IsLiveByIndex without re-locking the Registry.
    //
    // Returns count copied (out is cleared first).
    //
    // `mirror` is the Element's IsMirror() flag, captured under the same mutex.
    // The dead-Prop-Element reaper (prop_element_tracker) uses it to reap ONLY
    // local shadows: a wire mirror's teardown is the host's PropDestroy, never a
    // local reconciliation. Distinguishing by this per-Element flag (not by the
    // actor pointer) is robust against the engine recycling a purged actor's
    // address for a new keyed prop.
    struct ActorIdPair { void* actor; ElementId id; int32_t internalIdx; bool mirror; };
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
    // FREE LISTS: deque (not vector). Fresh (never-allocated) ids pop_back from the back; a FREED
    // id push_front to the FRONT so it is only re-issued after the fresh pool above it drains
    // (deferred FIFO reuse -- prevents the handle-recycling race where a transient clump grabs a
    // just-freed eid still bound to another prop's in-flight mirror on a peer). Both ends O(1)
    // (a vector's insert(begin) was O(n)).
    std::deque<ElementId> m_hostFree;    // host range: back=fresh, front=deferred-reuse
    std::deque<ElementId> m_localFree;   // ACTIVE peer band: back=fresh, front=deferred-reuse
    // Which peer-range band m_localFree currently holds (D9-2). 0 == the
    // pre-slot band (boot/seed window, slot not yet known); 1..kMaxPeers-1
    // == the client slot band activated by SetLocalPeerBand. The host never
    // changes this off 0 because it never calls AllocLocalId.
    uint8_t m_activeBand = 0;

    // Pre-populate m_hostFree with the full host range and m_localFree with
    // the pre-slot band only (one-time at construction). Called under
    // m_mutex (or during construction before any other thread exists).
    void RefillFreeStacks_();
};

}  // namespace coop::element
