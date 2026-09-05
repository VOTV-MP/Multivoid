// coop/element/registry.h -- the ElementId allocator and O(1) resolver, adapted from MTA's
// CElementArray (client side) and CElementIDs (server side) into one class: one address space
// shared by the host and the clients, partitioned by range, and the role decides which range a
// peer allocates from. Host range [0, kHostRangeSize), peer range [kHostRangeSize,
// kMaxElements). Ids resolve through a fixed array; allocation pops a per-range free list, and
// a freed id returns to the far end of it, so reuse is deferred behind the never-allocated
// pool. The mutex covers only the table and list operations, never engine reflection or
// Element construction.

#pragma once

#include "coop/element/element.h"

#include <cstddef>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace coop::element { class Element; }

namespace coop::element {

class Registry {
public:
    // Process-lifetime singleton, lazily constructed.
    static Registry& Get();

    // Allocate a fresh id in the host range, register `e` and write the id into it. Host role only.
    // Logs and returns kInvalidId when the range is exhausted, which is a bug, not an expected
    // state.
    ElementId AllocHostId(Element* e);

    // Allocate a fresh id in the peer range for a client-local element; either role may call it.
    // The peer range is sub-partitioned into per-slot bands so two client processes never mint
    // colliding ids (the host holds mirrors of both, and RegisterMirror refuses a populated slot).
    // Before SetLocalPeerBand, while the slot is unknown, ids come from the pre-slot band; nothing
    // allocated then is broadcast, so cross-client overlap of those ids is harmless.
    ElementId AllocLocalId(Element* e);

    // Activate this client's per-slot band for AllocLocalId; players::Registry::SetLocalPeerId
    // calls it once the slot is known. `slot` in [1, kMaxPeers); the host never calls it. Pre-slot
    // ids already handed out stay valid and freeable (FreeId returns them to the active list, where
    // they recycle harmlessly). Idempotent on the same slot; a no-op with a warning on slot 0 or
    // out of range.
    void SetLocalPeerBand(uint8_t slot);

    // Return an id to its free list and clear the table slot; called from the Element destructor.
    // No-op on kInvalidId; logs and skips an id that is already free, which would be a lifetime
    // bug.
    void FreeId(ElementId id);

    // Bind an Element to a host-allocated id received over the wire, for a client materialising a
    // mirror of an entity the host owns. Does not touch the free lists: the id was popped from the
    // host's pool, not this process's. Sets the table slot and stamps the Element's id and mirror
    // flag. Logs and returns false when the slot is already populated (a duplicate spawn or an id
    // collision, both upstream bugs) or the id is out of range or invalid. The mirror's destructor
    // calls UnregisterMirror through the flag, so the owner only drops its pointer.
    bool RegisterMirror(ElementId id, Element* e);

    // Drop a client-side mirror: clears the table slot without pushing to a free list, since
    // host-range ids belong to the host's allocation space. No-op on kInvalidId, out of range or
    // already empty.
    void UnregisterMirror(ElementId id);

    // O(1) lookup; nullptr for kInvalidId, out of range or not allocated.
    Element* Get(ElementId id) const;

    // The one actor-to-eid reverse for the whole registry, locals and mirrors alike, maintained by
    // Element::SetActor and the destructor through NoteActorRebind, so it always matches the live
    // binding. O(1); kInvalidId for a null or unbound actor. It does not validate engine liveness:
    // a caller holding the pointer across ticks re-validates with reflection::IsLiveByIndex.
    ElementId EidForActor(void* actor) const;

    // Called by Element::SetActor and the destructor: drop the old actor's entry if it still maps
    // to `id`, then point the new actor at `id`; the newest live binding wins on a recycled
    // address. nullptr clears. Only Element calls it.
    void NoteActorRebind(ElementId id, void* oldActor, void* newActor);

    // Range checks; kInvalidId is false on both.
    static bool IsHostId(ElementId id)  { return id < kHostRangeSize; }
    static bool IsLocalId(ElementId id) { return id >= kHostRangeSize && id < kMaxElements; }

    // Sender-role-aware validation of an inbound eid: a packet carrying an id in a range its
    // sender's role may not allocate from is forged or a relay-loop bug, and is dropped at the
    // boundary. A host sender may only name host-allocated ids, [1, kHostRangeSize); a client
    // sender only peer-allocated ones, [kHostRangeSize, kMaxElements). Receivers derive the
    // sender's role from the connection-stamped sender slot (0 is the host). The two specialised
    // forms serve sites where the role is fixed by the feature (entity spawns are host-only;
    // client-sourced prop spawns are client-only).
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

    // Currently allocated elements per range, for the object overlay.
    size_t HostCount() const;
    size_t LocalCount() const;

    // Copy the (actor, id, internal index, mirror flag) tuple of every allocated Element of the
    // type, under the mutex. The mutex protects the C++ Element lifetime only: the actor pointer is
    // a raw UObject* the garbage collector can free independently (a mass purge marks thousands of
    // props unreachable without a per-actor destroy, so their Elements and stale actor pointers
    // persist here). Consumers validate each actor with reflection::IsLiveByIndex and the captured
    // index, never with a check that dereferences first. Returns the count copied; `out` is cleared
    // first. The mirror flag lets the dead-Element reaper reap only local shadows (a wire mirror's
    // teardown is the host's destroy), and it holds when the engine recycles a purged actor's
    // address.
    struct ActorIdPair { void* actor; ElementId id; int32_t internalIdx; bool mirror; };
    size_t SnapshotActorsByType(ElementType t, std::vector<ActorIdPair>& out) const;

    // Deliberately no bulk Reset: each subsystem owns the lifetime of the Elements it allocates and
    // releases them on its own disconnect hook by draining its container, so the destructors free
    // the ids. A global reset would destroy other subsystems' elements and double-free ids when
    // their destructors ran later; MTA's CElementIDs has none for the same reason.

private:
    Registry();
    ~Registry();
    Registry(const Registry&)            = delete;
    Registry& operator=(const Registry&) = delete;

    mutable std::mutex m_mutex;
    Element* m_byId[kMaxElements] = {};   // index = ElementId; nullptr = free
    // The actor-to-eid reverse, guarded by m_mutex with m_byId, since they mutate together; locals
    // and mirrors.
    std::unordered_map<void*, ElementId> m_byActor;
    // Free lists as deques: fresh ids pop from the back, and a freed id goes to the front, so it is
    // re-issued only after the never-allocated pool above it drains, by which time any in-flight
    // message naming the old id has landed. Both ends O(1).
    std::deque<ElementId> m_hostFree;    // host range: back=fresh, front=deferred-reuse
    std::deque<ElementId> m_localFree;   // ACTIVE peer band: back=fresh, front=deferred-reuse
    // Which peer-range band m_localFree holds: 0 is the pre-slot band, 1..kMaxPeers-1 the client's
    // slot band after SetLocalPeerBand. The host stays on 0, since it never calls AllocLocalId.
    uint8_t m_activeBand = 0;

    // Pre-populate the host list with the whole host range and the local list with the pre-slot
    // band, once at construction.
    void RefillFreeStacks_();
};

// eid to the live actor of that element type, or nullptr (wrong type, unbound, or the engine
// slot recycled, checked by IsLiveByIndex). The resolve idiom for wire receivers. The type
// argument is the fail-closed half: an eid naming an Element of another kind resolves to
// nullptr, not to that kind's actor. Game thread only.
void* LiveActorOfType(ElementId eid, ElementType type);

// The Prop spelling, the idiom most wire receivers read.
inline void* LivePropActor(ElementId eid) {
    return LiveActorOfType(eid, ElementType::Prop);
}

}  // namespace coop::element
