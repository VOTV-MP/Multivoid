// coop/element/registry.cpp -- the unified element-id allocator and O(1) resolver. See the
// header for the public interface. Allocation pops the top of the per-range free stack. On
// construction both stacks are pre-populated with every id in their range, descending, so
// the first pop returns the lowest id (tidy for log inspection). The host range starts at
// id 1 (id 0 is the wire's invalid sentinel); the local range starts at the host range size.
// Lookup: a fixed array of 65536 pointers (about 512 KB), indexed with no hashing.

#include "coop/element/registry.h"

#include "coop/player/players_registry.h"  // kMaxPeers, the peer-band count
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"       // IsLiveByIndex (LivePropActor)

namespace coop::element {

namespace {

// An actor's identity in the object array, taken outside the registry's lock when it is bound: the
// slot the element recorded for it (Element::SetActor) and that slot's serial, allocated as the
// engine's weak pointer allocates it, so a same-address successor never matches. Reads the object
// array only, never the actor: an element may hold a sentinel address (the reaper's selftest).
struct ActorIdent {
    void*   actor = nullptr;
    int32_t idx = -1;
    int32_t serial = 0;
};
ActorIdent IdentOf(void* actor, int32_t idx) {
    ActorIdent ai;
    if (!actor) return ai;
    ai.actor = actor;
    ai.idx = idx;
    ai.serial = idx >= 0 ? ue_wrap::reflection::AllocateSlotSerial(idx) : 0;
    return ai;
}
// The peer range is split into one equal band per peer slot. Band 0 is the pre-slot band
// used during the boot and seed window before a client knows its slot; bands 1 and up are
// the per-client-slot exclusive bands. Slot 0 (the host) reuses band 0 as the pre-slot
// scratch, which is safe because the host never allocates local ids (its own elements come
// from the host range), so band 0 is only ever consumed by a client's pre-slot allocations.
constexpr uint32_t kPeerBandCount = coop::players::kMaxPeers;
constexpr uint32_t kSlotBandSize  =
    (kMaxElements - kHostRangeSize) / kPeerBandCount;  // 32768/4 = 8192

// The [base, end) of the band for a band index (0 is pre-slot, 1 and up are client slots).
// The last band absorbs any remainder so the union exactly covers the peer range.
constexpr ElementId BandBase(uint8_t band) {
    return kHostRangeSize + static_cast<ElementId>(band) * kSlotBandSize;
}
constexpr ElementId BandEnd(uint8_t band) {
    return (band + 1u == kPeerBandCount) ? kMaxElements
                                         : BandBase(band) + kSlotBandSize;
}
}  // namespace

// The latch flipper in element.cpp.
void NotifyRegistryShuttingDown();

Registry::Registry() {
    RefillFreeStacks_();  // m_hostFree/m_localFree are deques -- no reserve needed
}

Registry::~Registry() {
    // Tell every element destructor running after this point (during process-exit teardown of
    // namespace-scope owner containers) to skip the free on this now-dead storage.
    NotifyRegistryShuttingDown();
}

Registry& Registry::Get() {
    // A function-local static: thread-safe initialisation.
    static Registry instance;
    return instance;
}

void Registry::RefillFreeStacks_() {
    // Descending push so the first pop returns the lowest id in each range (readable logs when
    // only a handful of ids are live). The host range starts at 1, not 0: id 0 is the wire's
    // invalid sentinel, so receivers can drop a zero id without losing a real element.
    m_hostFree.clear();
    for (ElementId id = kHostRangeSize; id-- > 1;) {
        m_hostFree.push_back(id);
    }
    // Seed the local free stack with the pre-slot band only, not the whole peer range. A client
    // switches to its exclusive slot band once its slot is assigned. Pre-filling the whole range
    // would let a pre-slot id later be re-issued from the slot band while the pre-slot element
    // still holds it: an in-process id collision.
    m_localFree.clear();
    m_activeBand = 0;
    for (ElementId id = BandEnd(0); id-- > BandBase(0);) {
        m_localFree.push_back(id);
    }
}

void Registry::SetLocalPeerBand(uint8_t slot) {
    if (slot == 0 || slot >= coop::players::kMaxPeers) {
        UE_LOGW("element::Registry: SetLocalPeerBand(%u) invalid -- valid client "
                "slots are [1, %u); ignoring",
                static_cast<unsigned>(slot),
                static_cast<unsigned>(coop::players::kMaxPeers));
        return;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_activeBand == slot) return;  // idempotent re-assign on the same slot
    // Replace the local free stack with the slot's exclusive band. Pre-slot ids still alive are
    // not on the free stack (popped at allocation and not yet freed), so clearing the stack
    // cannot orphan a live element; their eventual free pushes them onto the new stack, where
    // they recycle harmlessly (the free nulls the slot before the push, so a re-issue finds it
    // empty). Already-freed pre-slot ids on the old stack are discarded: a bounded one-time leak
    // of at most the pre-slot allocations destroyed before the handshake completed.
    m_localFree.clear();
    m_activeBand = slot;
    const ElementId base = BandBase(slot);
    const ElementId end  = BandEnd(slot);
    for (ElementId id = end; id-- > base;) {
        m_localFree.push_back(id);
    }
    UE_LOGI("element::Registry: activated peer band for slot %u -> [%u, %u) "
            "(%u ids)", static_cast<unsigned>(slot), base, end, end - base);
}

ElementId Registry::AllocHostId(Element* e) {
    if (!e) return kInvalidId;
    const ActorIdent ai = IdentOf(e->GetActor(), e->GetInternalIdx());
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_hostFree.empty()) {
        UE_LOGW("element::Registry: host range exhausted (32768 active elements); "
                "AllocHostId returning kInvalidId -- this indicates an Element lifetime bug");
        return kInvalidId;
    }
    const ElementId id = m_hostFree.back();
    m_hostFree.pop_back();
    if (m_byId[id]) {
        // Defensive: a free-stack id whose slot is already occupied means the free stack diverged
        // from the table (a lifetime or double-free bug upstream). Do not clobber the existing
        // element or hand out a doubly owned id: fail the allocation; the caller treats an invalid
        // id as failure and drops its new element without freeing this foreign id. The corrupt id
        // is dropped, not re-pushed. Never fires in correct operation: the free nulls the slot
        // before it pushes the id.
        UE_LOGE("element::Registry: AllocHostId popped id=%u but its slot is "
                "occupied (free-stack/m_byId divergence) -- failing alloc", id);
        return kInvalidId;
    }
    m_byId[id] = e;
    e->SetId_(id);
    if (ai.actor) m_byActor[ai.actor] = {id, ai.idx, ai.serial};  // actor set before id -> reverse here
    return id;
}

ElementId Registry::AllocLocalId(Element* e) {
    if (!e) return kInvalidId;
    const ActorIdent ai = IdentOf(e->GetActor(), e->GetInternalIdx());
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_localFree.empty()) {
        UE_LOGW("element::Registry: peer range exhausted (32768 active elements); "
                "AllocLocalId returning kInvalidId -- this indicates an Element lifetime bug");
        return kInvalidId;
    }
    const ElementId id = m_localFree.back();
    m_localFree.pop_back();
    if (m_byId[id]) {
        // Defensive, see AllocHostId: a popped peer-range id whose slot is occupied is a
        // divergence; fail the allocation rather than clobber. Never fires normally.
        UE_LOGE("element::Registry: AllocLocalId popped id=%u but its slot is "
                "occupied (free-stack/m_byId divergence) -- failing alloc", id);
        return kInvalidId;
    }
    m_byId[id] = e;
    e->SetId_(id);
    if (ai.actor) m_byActor[ai.actor] = {id, ai.idx, ai.serial};  // actor set before id -> reverse here
    return id;
}

void Registry::FreeId(ElementId id) {
    if (id == kInvalidId || id >= kMaxElements) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_byId[id]) {
        UE_LOGW("element::Registry: FreeId(%u) -- slot already empty (double-free?)", id);
        return;
    }
    m_byId[id] = nullptr;
    // FIFO reuse: push the freed id to the front (bottom) of the stack, not the back. Allocation
    // pops the back, so a freed id is only re-issued after the entire never-allocated pool above
    // it is exhausted, which defers reuse by hours of normal allocation, long enough for every
    // in-flight cross-peer message referencing the old id (the unreliable pose stream, a
    // reliable destroy or convert) to drain before the id is handed to a new entity. Immediate
    // reuse let a transient trash clump take a low id still bound to another prop's mirror on
    // the peer, so the held-clump pose stream drove the wrong actor: a grabbed pile morphing
    // into a bottle, a cassette, a clump, and cross-peer duplicates. The front push on the deque
    // is O(1).
    if (IsHostId(id)) {
        m_hostFree.push_front(id);
    } else {
        m_localFree.push_front(id);
    }
}

bool Registry::RegisterMirror(ElementId id, Element* e) {
    if (id == kInvalidId || id >= kMaxElements) {
        UE_LOGW("element::Registry: RegisterMirror(id=%u) out of range -- rejecting", id);
        return false;
    }
    if (!e) {
        UE_LOGW("element::Registry: RegisterMirror(id=%u, nullptr) -- rejecting", id);
        return false;
    }
    const ActorIdent ai = IdentOf(e->GetActor(), e->GetInternalIdx());
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_byId[id]) {
        UE_LOGW("element::Registry: RegisterMirror(id=%u) -- slot already populated by existing element (duplicate spawn? wire id collision?)",
                id);
        return false;
    }
    m_byId[id] = e;
    e->SetId_(id);
    e->SetMirror_(true);
    if (ai.actor) m_byActor[ai.actor] = {id, ai.idx, ai.serial};  // mirror actor often set before Install
    return true;
}

void Registry::UnregisterMirror(ElementId id) {
    if (id == kInvalidId || id >= kMaxElements) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_byId[id]) {
        // Acceptable: the disconnect may have drained the mirror table before the element
        // destructor ran. Logged at info level so a true double-free is still visible outside a
        // drain.
        UE_LOGI("element::Registry: UnregisterMirror(%u) -- slot already empty (drained by OnDisconnect?)",
                id);
        return;
    }
    m_byId[id] = nullptr;
    // Intentionally no free-stack push: the id is in the host's allocation space and was never
    // popped from this peer's free stack.
}

Element* Registry::Get(ElementId id) const {
    if (id == kInvalidId || id >= kMaxElements) return nullptr;
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_byId[id];
}

ElementId Registry::EidForActor(void* actor) const {
    if (!actor) return kInvalidId;
    ActorBinding b{};
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_byActor.find(actor);
        if (it == m_byActor.end()) return kInvalidId;
        b = it->second;
    }
    // The slot still holds this pointer and its serial is the one taken at the bind: the bound actor,
    // not a successor at its address. The engine resets a slot's serial once its object is destroyed.
    namespace R = ue_wrap::reflection;
    if (R::ObjectAt(b.idx) != actor || R::SlotSerial(b.idx) != b.serial) return kInvalidId;
    return b.id;
}

void Registry::NoteActorRebind(ElementId id, void* oldActor, void* newActor, int32_t newIdx) {
    if (oldActor == newActor) return;
    const ActorIdent ai = IdentOf(newActor, newIdx);
    std::lock_guard<std::mutex> lk(m_mutex);
    if (oldActor) {
        auto it = m_byActor.find(oldActor);
        // Only erase if it still points at us: a recycled address already re-pointed to a newer
        // element must not be clobbered by our teardown.
        if (it != m_byActor.end() && it->second.id == id) m_byActor.erase(it);
    }
    if (ai.actor) m_byActor[ai.actor] = {id, ai.idx, ai.serial};  // newest live binding wins
}

size_t Registry::HostCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    // The host range owns ids from 1 to the range size, exclusive: the range size minus one
    // allocatable ids (id 0 is the wire's invalid sentinel, never on the free stack). Allocated
    // is capacity minus free.
    return (kHostRangeSize - 1) - m_hostFree.size();
}

size_t Registry::LocalCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    // The local free stack holds the currently active band only, so the allocated count is that
    // band's capacity minus the free remainder. Clamped at 0: after a band switch, freed pre-slot
    // ids recycle onto the stack and can transiently push its size above the band capacity (a
    // bounded, harmless overshoot), so avoid the unsigned underflow.
    const size_t bandCapacity = BandEnd(m_activeBand) - BandBase(m_activeBand);
    return m_localFree.size() >= bandCapacity ? 0
                                              : bandCapacity - m_localFree.size();
}

size_t Registry::SnapshotActorsByType(ElementType t,
                                      std::vector<ActorIdPair>& out) const {
    out.clear();
    std::lock_guard<std::mutex> lk(m_mutex);
    for (ElementId id = 0; id < kMaxElements; ++id) {
        Element* e = m_byId[id];
        if (e && e->GetType() == t) {
            out.push_back({e->GetActor(), id, e->GetInternalIdx(), e->IsMirror()});
        }
    }
    return out.size();
}

void* LiveActorOfType(ElementId eid, ElementType type) {
    if (!eid) return nullptr;
    Element* e = Registry::Get().Get(eid);
    if (!e || e->GetType() != type) return nullptr;
    void* a = e->GetActor();
    if (!a || !ue_wrap::reflection::IsLiveByIndex(a, e->GetInternalIdx())) return nullptr;
    return a;
}

}  // namespace coop::element
