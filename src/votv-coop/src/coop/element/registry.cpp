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

#include "ue_wrap/log.h"

namespace coop::element {

// Forward decl of the latch-flipper in element.cpp (audit fix 2026-05-28).
void NotifyRegistryShuttingDown();

Registry::Registry() {
    m_hostFree.reserve(kHostRangeSize);
    m_localFree.reserve(kMaxElements - kHostRangeSize);
    RefillFreeStacks_();
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
    m_localFree.clear();
    for (ElementId id = kMaxElements; id-- > kHostRangeSize;) {
        m_localFree.push_back(id);
    }
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
    m_byId[id] = e;
    e->SetId_(id);
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
    m_byId[id] = e;
    e->SetId_(id);
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
    if (IsHostId(id)) {
        m_hostFree.push_back(id);
    } else {
        m_localFree.push_back(id);
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

size_t Registry::HostCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    // Host range owns ids [1, kHostRangeSize) -- that's kHostRangeSize - 1
    // allocatable ids. (id 0 is reserved as wire-invalid; never on the
    // free stack.) Allocated count = capacity - free.
    return (kHostRangeSize - 1) - m_hostFree.size();
}

size_t Registry::LocalCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return (kMaxElements - kHostRangeSize) - m_localFree.size();
}

size_t Registry::SnapshotActorsByType(ElementType t,
                                      std::vector<ActorIdPair>& out) const {
    out.clear();
    std::lock_guard<std::mutex> lk(m_mutex);
    for (ElementId id = 0; id < kMaxElements; ++id) {
        Element* e = m_byId[id];
        if (e && e->GetType() == t) {
            out.push_back({e->GetActor(), id});
        }
    }
    return out.size();
}

}  // namespace coop::element
