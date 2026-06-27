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

// Static-destruction-order safety (audit fix 2026-05-28): the Element owner
// containers -- the MirrorManager<T>::Instance() singletons (Player / Npc /
// Prop) plus any remaining namespace-scope owners -- are function-local /
// namespace-scope statics whose teardown order relative to the Registry
// singleton (also a Meyers static-local) is not guaranteed. If an owner
// drains AFTER the Registry is destroyed, the Element destructor would call
// Registry::Get().FreeId()/UnregisterMirror() on torn-down storage. This latch
// flips when the Registry singleton is about to be destroyed (set by
// Registry's destructor) so subsequent ~Element() calls skip the Registry
// call rather than UAF -- order-independent-safe either way.
std::atomic<bool> g_registryShuttingDown{false};

void NotifyRegistryShuttingDown() {
    g_registryShuttingDown.store(true, std::memory_order_release);
}

Element::Element(ElementType type) : m_type(type) {
    // No Registry call -- the subsystem allocates explicitly after construction.
    // m_id stays kInvalidId until then.
}

// Maintains the Registry's unified actor->eid reverse alongside the local
// fields, so Registry::EidForActor stays consistent with the live binding.
// Non-inline (vs the header) because it must reach the (incomplete-in-header)
// Registry. Called at every bind/rebind/clear across all element types.
void Element::SetActor(void* a, int32_t internalIdx) {
    void* old = m_actor;
    m_actor       = a;
    m_internalIdx = internalIdx;
    if (m_id != kInvalidId && old != a &&
        !g_registryShuttingDown.load(std::memory_order_acquire)) {
        Registry::Get().NoteActorRebind(m_id, old, a);
    }
}

Element::~Element() {
    if (m_id == kInvalidId) return;
    // Static-destruction-order safety: skip Registry calls if the Registry has
    // already been torn down. The OS reclaims memory on process exit.
    if (g_registryShuttingDown.load(std::memory_order_acquire)) return;
    // Drop our actor from the unified reverse before releasing the id.
    if (m_actor) Registry::Get().NoteActorRebind(m_id, m_actor, nullptr);
    // Mirrors borrowed the id from the host's allocation space; releasing the
    // id back to the local free stack would corrupt it with foreign entries
    // over a long session. UnregisterMirror just clears m_byId[id].
    if (m_mirror) Registry::Get().UnregisterMirror(m_id);
    else          Registry::Get().FreeId(m_id);
}

}  // namespace coop::element
