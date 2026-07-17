// coop/element/element_deleter.cpp -- see coop/element/element_deleter.h.

#include "coop/element/element_deleter.h"

#include "ue_wrap/core/log.h"

namespace coop::element {

ElementDeleter& ElementDeleter::Get() {
    static ElementDeleter instance;
    return instance;
}

void ElementDeleter::Enqueue(std::unique_ptr<Element> element) {
    if (!element) return;
    // Flag dead BEFORE the element becomes reachable to a concurrent Flush /
    // resolve gate. ~Element does not clear it; the element is being destroyed.
    element->SetBeingDeleted(true);
    std::lock_guard<std::mutex> lk(m_mutex);
    m_pending.push_back(std::move(element));
}

size_t ElementDeleter::Flush() {
    std::vector<std::unique_ptr<Element>> drained;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_pending.empty()) return 0;  // steady-state fast path
        drained.swap(m_pending);
    }
    const size_t n = drained.size();
    // Dtors fire here, OUTSIDE m_mutex: each ~Element takes the Registry mutex
    // (FreeId / UnregisterMirror). Holding m_mutex across them would nest
    // deleter-mutex -> registry-mutex; keeping it a leaf avoids any ABBA risk
    // with the rest of the element layer (MirrorManager uses the same rule).
    drained.clear();
    UE_LOGI("element::ElementDeleter: flushed %zu deferred element(s)", n);
    return n;
}

size_t ElementDeleter::PendingCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_pending.size();
}

}  // namespace coop::element
