// coop/element/element.cpp -- Element ctor/dtor.
//
// Element is allocated with kInvalidId; the owning subsystem then calls
// Registry::AllocHostId(e) on the host side OR Registry::AllocLocalId(e)
// on the local-peer-only path BEFORE first use. The destructor releases
// the id back to the free stack.
//
// Design tradeoff vs MTA: MTA's CClientEntity ctor takes an ElementID
// parameter so the ctor itself registers. Our PoC uses an explicit
// post-construction Alloc call so the Element type has zero role-awareness
// -- the subsystem (which knows its role) chooses which range to allocate
// from. Per the audit section 4.1.
//
// See coop/element/element.h for the public interface.

#include "coop/element/element.h"

#include "coop/element/registry.h"

#include <atomic>

namespace coop::element {

// Static-destruction-order safety (audit fix 2026-05-28): namespace-scope
// owner containers in prop_lifecycle (g_propElementsById) and elsewhere
// destruct AFTER the Registry singleton (Meyers static-local constructed
// during runtime). When those owners drain at process exit, the Element
// destructor would call Registry::Get().FreeId() on torn-down storage.
// This latch flips when the Registry singleton is about to be destroyed
// (set by Registry's destructor) so subsequent ~Element() calls skip
// FreeId rather than UAF.
std::atomic<bool> g_registryShuttingDown{false};

void NotifyRegistryShuttingDown() {
    g_registryShuttingDown.store(true, std::memory_order_release);
}

Element::Element(ElementType type) : m_type(type) {
    // No Registry call -- the subsystem allocates explicitly after construction.
    // m_id stays kInvalidId until then.
}

Element::~Element() {
    if (m_id == kInvalidId) return;
    // Static-destruction-order safety: skip Registry calls if the Registry has
    // already been torn down. The OS reclaims memory on process exit.
    if (g_registryShuttingDown.load(std::memory_order_acquire)) return;
    // Mirrors borrowed the id from the host's allocation space; releasing the
    // id back to the local free stack would corrupt it with foreign entries
    // over a long session. UnregisterMirror just clears m_byId[id].
    if (m_mirror) Registry::Get().UnregisterMirror(m_id);
    else          Registry::Get().FreeId(m_id);
}

}  // namespace coop::element
