// coop/element/registry.cpp -- the unified ElementId allocator + O(1) resolver.
//
// See coop/element/registry.h for the public interface.
//
// Allocation: pop the top of the per-range free stack (LIFO). On construction
// both stacks are pre-populated with every id in their range (descending,
// so the FIRST pop returns the LOWEST id -- aesthetically tidy for log
// inspection during early bring-up). Host range starts at id 1 (id 0 is
// reserved as the wire-protocol "invalid" sentinel); local range starts at
// kHostRangeSize.
//
// Lookup: m_byId is a fixed-size array of 65536 pointers (~512 KB on 64-bit).
// Indexed access is O(1) with no hashing. Memory cost is bounded and trivial
// relative to engine working set.

#include "coop/element/registry.h"

#include "coop/player/players_registry.h"  // kMaxPeers (peer-band count, D9-2)
#include "ue_wrap/log.h"

namespace coop::element {

namespace {
// D9-2 (PR-FOUNDATION Tier 2): the peer range [kHostRangeSize, kMaxElements)
// is split into kMaxPeers equal bands. Band index 0 is the "pre-slot" band
// used during the boot/seed window before a client knows its slot; band
// indices 1..kMaxPeers-1 are the per-client-slot exclusive bands. Slot 0
// (kPeerIdHost) reuses band 0 as the pre-slot scratch -- safe because the
// host NEVER calls AllocLocalId (it allocates its own elements from the
// host range via AllocHostId), so band 0 is only ever consumed by
// pre-slot-window allocations on a client.
constexpr uint32_t kPeerBandCount = coop::players::kMaxPeers;
constexpr uint32_t kSlotBandSize  =
    (kMaxElements - kHostRangeSize) / kPeerBandCount;  // 32768/4 = 8192

// [base, end) of the band for the given band index (0 = pre-slot,
// 1..kMaxPeers-1 = client slots). The last band absorbs any remainder so
// the union exactly covers the peer range.
constexpr ElementId BandBase(uint8_t band) {
    return kHostRangeSize + static_cast<ElementId>(band) * kSlotBandSize;
}
constexpr ElementId BandEnd(uint8_t band) {
    return (band + 1u == kPeerBandCount) ? kMaxElements
                                         : BandBase(band) + kSlotBandSize;
}
}  // namespace

// Forward decl of the latch-flipper in element.cpp (audit fix 2026-05-28).
void NotifyRegistryShuttingDown();

Registry::Registry() {
    RefillFreeStacks_();  // m_hostFree/m_localFree are deques -- no reserve needed
}

Registry::~Registry() {
    // Tell every Element destructor running AFTER this point (i.e. during
    // process-exit teardown of namespace-scope owner containers) to skip
    // FreeId on this now-dead storage. UAF prevention -- audit fix 2026-05-28.
    NotifyRegistryShuttingDown();
}

Registry& Registry::Get() {
    // Meyers singleton -- thread-safe init since C++11.
    static Registry instance;
    return instance;
}

void Registry::RefillFreeStacks_() {
    // Descending push so the first pop returns the lowest id in each range
    // (ergonomic for log readability when only a handful of ids are live).
    //
    // Host range starts at 1, NOT 0 (audited 2026-05-28): ElementId 0 is
    // reserved as the wire-protocol "invalid" sentinel matching
    // protocol.h's EntitySpawnPayload.sessionId contract. Inc3 receiver
    // code can defensively drop sessionId==0 without losing real elements.
    m_hostFree.clear();
    for (ElementId id = kHostRangeSize; id-- > 1;) {
        m_hostFree.push_back(id);
    }
    // D9-2: seed m_localFree with the PRE-SLOT band (band 0) only -- NOT the
    // whole peer range. A client switches to its exclusive slot band via
    // SetLocalPeerBand once AssignPeerSlot delivers its slot. Pre-filling the
    // whole range would let a pre-slot id (e.g. 32768) later be re-issued from
    // the slot band [32768, ...) while the pre-slot element still holds it ->
    // intra-process m_byId collision.
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
    // Replace m_localFree with the slot's exclusive band. Pre-slot ids still
    // alive in m_byId are NOT on the free stack (they were popped at alloc and
    // not yet freed), so clearing the stack here cannot orphan a live element;
    // their eventual FreeId pushes them back onto m_localFree (now the slot
    // band) where they recycle harmlessly (FreeId clears m_byId before the
    // push, so a re-issue finds an empty slot). Already-freed pre-slot ids
    // sitting on the old m_localFree are simply discarded -- a bounded
    // one-time leak of at most the pre-slot allocations destroyed before the
    // handshake completed (rare; world-load props normally outlive it).
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
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_hostFree.empty()) {
        UE_LOGW("element::Registry: host range exhausted (32768 active elements); "
                "AllocHostId returning kInvalidId -- this indicates an Element lifetime bug");
        return kInvalidId;
    }
    const ElementId id = m_hostFree.back();
    m_hostFree.pop_back();
    if (m_byId[id]) {
        // Defensive (PR-FOUNDATION-3 review 2026-05-30): a free-stack id whose
        // m_byId slot is already occupied means the free stack diverged from
        // m_byId (a lifetime / double-free bug upstream). Do NOT clobber the
        // existing element or hand out a doubly-owned id -- fail the alloc; the
        // caller (e.g. MirrorManager::AllocAndInstall) treats kInvalidId as
        // failure and drops its new element WITHOUT freeing this foreign id. The
        // corrupt id is dropped (not re-pushed). NEVER fires in correct
        // operation: FreeId nulls the slot before it pushes the id.
        UE_LOGE("element::Registry: AllocHostId popped id=%u but its slot is "
                "occupied (free-stack/m_byId divergence) -- failing alloc", id);
        return kInvalidId;
    }
    m_byId[id] = e;
    e->SetId_(id);
    if (void* a = e->GetActor()) m_byActor[a] = id;  // actor set before id -> reverse here
    return id;
}

ElementId Registry::AllocLocalId(Element* e) {
    if (!e) return kInvalidId;
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_localFree.empty()) {
        UE_LOGW("element::Registry: peer range exhausted (32768 active elements); "
                "AllocLocalId returning kInvalidId -- this indicates an Element lifetime bug");
        return kInvalidId;
    }
    const ElementId id = m_localFree.back();
    m_localFree.pop_back();
    if (m_byId[id]) {
        // Defensive (PR-FOUNDATION-3 review 2026-05-30): see AllocHostId. A
        // popped peer-range id whose slot is occupied = free-stack/m_byId
        // divergence; fail the alloc rather than clobber. Never fires normally.
        UE_LOGE("element::Registry: AllocLocalId popped id=%u but its slot is "
                "occupied (free-stack/m_byId divergence) -- failing alloc", id);
        return kInvalidId;
    }
    m_byId[id] = e;
    e->SetId_(id);
    if (void* a = e->GetActor()) m_byActor[a] = id;  // actor set before id -> reverse here
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
    // FIFO REUSE (v52, 2026-06-09): push the freed id to the FRONT (bottom) of the stack, NOT the
    // back. Alloc pops the back, so a freed id is only re-issued AFTER the entire never-allocated
    // pool above it is exhausted (~28k host / ~8k peer ids). This DEFERS reuse by hours of normal
    // allocation -- long enough that every in-flight cross-peer message referencing the old id
    // (the unreliable PropPose stream, a reliable PropDestroy/PropConvert) has drained before the
    // id is handed to a new entity. The prior LIFO push_back re-issued a just-freed id IMMEDIATELY,
    // which let a transient trash CLUMP grab a low eid (e.g. 126) still bound to another prop's
    // mirror on the peer -> the held-clump pose stream drove the WRONG actor = the "grabbed pile
    // turns into a bottle then a cassette then a clump" morphing + cross-peer dupes (proven from
    // the 2026-06-09 logs: GRAB-IN eid=126 resolved to an Aprop mirror, not the clump).
    // push_front on the std::deque free-list is O(1) (perf audit W-1 2026-06-10: an earlier
    // comment described a discarded vector-shift iteration).
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
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_byId[id]) {
        UE_LOGW("element::Registry: RegisterMirror(id=%u) -- slot already populated by existing element (duplicate spawn? wire id collision?)",
                id);
        return false;
    }
    m_byId[id] = e;
    e->SetId_(id);
    e->SetMirror_(true);
    if (void* a = e->GetActor()) m_byActor[a] = id;  // mirror actor often set before Install -> reverse here
    return true;
}

void Registry::UnregisterMirror(ElementId id) {
    if (id == kInvalidId || id >= kMaxElements) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_byId[id]) {
        // Acceptable: OnDisconnect may have drained the mirror table before
        // the Element dtor ran. Log at info-level so a true double-free is
        // still visible if the table is not in disconnect-drain mode.
        UE_LOGI("element::Registry: UnregisterMirror(%u) -- slot already empty (drained by OnDisconnect?)",
                id);
        return;
    }
    m_byId[id] = nullptr;
    // Intentionally NO free-stack push: the id is in the host's allocation
    // space and was never popped from this peer's free stack.
}

Element* Registry::Get(ElementId id) const {
    if (id == kInvalidId || id >= kMaxElements) return nullptr;
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_byId[id];
}

ElementId Registry::EidForActor(void* actor) const {
    if (!actor) return kInvalidId;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_byActor.find(actor);
    return (it == m_byActor.end()) ? kInvalidId : it->second;
}

void Registry::NoteActorRebind(ElementId id, void* oldActor, void* newActor) {
    if (oldActor == newActor) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    if (oldActor) {
        auto it = m_byActor.find(oldActor);
        // Only erase if it still points at US -- a recycled address already
        // re-pointed to a newer Element must not be clobbered by our teardown.
        if (it != m_byActor.end() && it->second == id) m_byActor.erase(it);
    }
    if (newActor) m_byActor[newActor] = id;  // newest live binding wins
}

size_t Registry::HostCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    // Host range owns ids [1, kHostRangeSize) -- that's kHostRangeSize - 1
    // allocatable ids. (id 0 is reserved as wire-invalid; never on the
    // free stack.) Allocated count = capacity - free.
    return (kHostRangeSize - 1) - m_hostFree.size();
}

size_t Registry::LocalCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    // D9-2: m_localFree holds the currently-active band only, so the
    // allocated count is that band's capacity minus the free remainder.
    // Clamp at 0: after a band switch, freed pre-slot ids recycle onto
    // m_localFree and can transiently push its size above the band
    // capacity (a bounded, harmless overshoot) -- avoid size_t underflow.
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

}  // namespace coop::element
