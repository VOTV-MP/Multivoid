// coop/element/mirror_manager.h -- the per-type mirror Element manager, one instance per
// Element subclass: it owns the id-to-element map, the register and drain lifecycle, and the
// snapshot iteration. The register order is fixed so the type mutex and the Registry mutex
// never nest: emplace into the owner map under the type mutex, release it, register with the
// global Registry, and on failure drain the entry so the unique_ptr destructs outside the lock;
// teardown swaps the unique_ptr out under the mutex and lets the destructor (which
// unregisters) run outside it. Every method takes the per-instance mutex and is safe from any
// thread; destructor work always runs outside it. Iteration is a snapshot of raw pointers
// copied under the lock and walked without it; there is no ForEach, since a callback under the
// mutex would invite a lock-order inversion. Each manager is a process-lifetime singleton.

#pragma once

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "ue_wrap/core/log.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// The one gateway allowed to call Install, defined in identity_create.cpp: a wire mirror can be
// bound only through coop::element's create funnel, and a feature file cannot install its own.
namespace coop::element { struct MirrorInstallAccess; }

namespace coop::element {

template <typename T>
class MirrorManager {
public:
    static MirrorManager& Instance() {
        static MirrorManager s_inst;
        return s_inst;
    }

    MirrorManager(const MirrorManager&)            = delete;
    MirrorManager& operator=(const MirrorManager&) = delete;

    // Register a mirror at `wireEid`. False on a zero or invalid eid, a duplicate (the caller's
    // mirror is discarded outside the lock, and its destructor early-returns), or a Registry
    // failure (rolled back the same way). `ownerSlot` tags the mirror with its originating peer
    // slot so a per-slot disconnect can drain that peer's mirrors; -1 leaves it untagged, for
    // host-authoritative mirrors that vacate only on a full teardown.
  private:
    // Sealed: Install is reachable only through the friend below, so binding a wire mirror outside
    // coop::element is a compile error. AllocAndInstall and the teardown stay public.
    friend struct coop::element::MirrorInstallAccess;
    bool Install(ElementId wireEid, std::unique_ptr<T> mirror, int ownerSlot = -1) {
        if (wireEid == 0u) return false;            // wire sentinel "no Element"
        if (wireEid == kInvalidId) {
            UE_LOGW("MirrorManager: Install with kInvalidId rejected (sender bug?)");
            return false;
        }
        if (!mirror) {
            UE_LOGW("MirrorManager: Install eid=0x%08x called with null mirror -- skip",
                    wireEid);
            return false;
        }
        // Tagged before publishing under the mutex, so the disconnect drain sees a consistent pair.
        mirror->SetOwnerSlot(static_cast<int8_t>(ownerSlot));
        T* raw = nullptr;
        // The emplace and the duplicate check under the type mutex, released before the Registry
        // call.
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_byId.count(wireEid) > 0) {
                // Idempotent: a re-spawn for an eid already mirrored. The local mirror falls out of
                // scope, and its destructor early-returns on an unset id.
                return false;
            }
            auto [it, ok] = m_byId.emplace(wireEid, std::move(mirror));
            if (!ok) return false;
            raw = it->second.get();
        }
        // The Registry registration, no type mutex held.
        if (!Registry::Get().RegisterMirror(wireEid, raw)) {
            UE_LOGE("MirrorManager: Registry::RegisterMirror(eid=0x%08x) FAILED -- "
                    "draining owner map (slot collision?)",
                    wireEid);
            // The rollback: the entry is drained, and the losing unique_ptr destructs outside the
            // mutex.
            std::unique_ptr<T> losing;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                auto it = m_byId.find(wireEid);
                if (it != m_byId.end()) {
                    losing = std::move(it->second);
                    m_byId.erase(it);
                }
            }
            return false;
        }
        return true;
    }

  public:
    // The authoritative-role path: allocates a fresh id from the Registry (the host range when
    // `isHost`, the peer range otherwise), binds the element to it and emplaces under the type
    // mutex. Returns the id, or kInvalidId on a null element, Registry exhaustion or a collision;
    // in every failure the element is consumed. The element's mirror flag stays false, so its
    // destructor frees the id rather than unregistering a mirror; that one flag lets the unified
    // drain release both kinds. The Registry mutex and the type mutex are never held together,
    // here or in Install, and every drain destructs outside the type mutex, so the element layer
    // has no lock nesting at all. For most types a process holds host-minted or wire-mirror
    // elements, never both; Prop mixes them (a peer's own keyed props and the other peer's
    // mirrors), which is still correct because the flag is per element, but it forbids a bulk
    // DrainAll on disconnect, since the locals must survive a reconnect in the same process: the
    // Prop path uses DrainMirrorsOnly.
    ElementId AllocAndInstall(std::unique_ptr<T> element, bool isHost) {
        if (!element) return kInvalidId;
        const ElementId eid = isHost ? Registry::Get().AllocHostId(element.get())
                                     : Registry::Get().AllocLocalId(element.get());
        if (eid == kInvalidId) {
            // Registry exhausted. The allocators set the element's id only on success, so the
            // destructor skips the free.
            return kInvalidId;
        }
        bool collided = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_byId.count(eid) == 0)
                m_byId.emplace(eid, std::move(element));
            else
                collided = true;
        }
        if (collided) {
            // A freshly allocated id already in the owner map is a free-stack or lifetime bug
            // upstream; the element destructs outside the mutex, and it is logged.
            UE_LOGE("MirrorManager: AllocAndInstall fresh eid=%u already in owner "
                    "map -- dropping new element (free-stack/lifetime bug upstream)",
                    eid);
            return kInvalidId;
        }
        return eid;
    }

    // Drop the mirror at `wireEid`; the destructor fires outside the mutex. Idempotent.
    void Drop(ElementId wireEid) {
        (void)Take(wireEid);  // discard the returned unique_ptr; dtor fires here
    }

    // As Drop, but returns the unique_ptr so the caller can read the Element first. The caller lets
    // it fall out of scope (the destructor unregisters, with the type mutex released) or
    // re-installs it. Null when missing.
    std::unique_ptr<T> Take(ElementId wireEid) {
        if (wireEid == 0u || wireEid == kInvalidId) return {};
        std::unique_ptr<T> drained;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            auto it = m_byId.find(wireEid);
            if (it == m_byId.end()) return {};
            drained = std::move(it->second);
            m_byId.erase(it);
        }
        // The type mutex is released; the caller owns `drained`.
        return drained;
    }

    // O(1) lookup, nullptr when absent. Any thread that does not need the pointer to outlive a
    // concurrent Drop; a caller reading actor state re-validates liveness.
    T* Get(ElementId wireEid) {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_byId.find(wireEid);
        return (it == m_byId.end()) ? nullptr : it->second.get();
    }

    // A copy of the raw pointers under the lock, iterated without it: the shape for any iteration
    // that does engine work.
    void Snapshot(std::vector<T*>& out) const {
        out.clear();
        std::lock_guard<std::mutex> lk(m_mutex);
        out.reserve(m_byId.size());
        for (const auto& kv : m_byId) out.push_back(kv.second.get());
    }

    // Drain everything; the count is returned, and the unique_ptrs destruct after the lock
    // releases. Only for a pure manager; a mixed one (Prop) uses DrainMirrorsOnly, or its locals
    // would be destroyed.
    size_t DrainAll() {
        std::unordered_map<ElementId, std::unique_ptr<T>> drained;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            drained.swap(m_byId);
        }
        return drained.size();
        // The destructors fire here, outside the mutex.
    }

    // Drain only the wire mirrors, leaving the locally allocated elements in place: the mirrors are
    // session state bound to a remote authority's ids, the locals are shadows of this peer's own
    // persistent props that must survive a reconnect (the keyed-prop seed scan runs once per
    // process). The mirror flag read here is not made safe by the type mutex: it is written under
    // the Registry mutex after Install released ours. What makes it correct is game-thread
    // serialisation: for the one mixed type the mirror producer and this drain both run on the
    // game thread inside the pump tick. Moving the mirror install off the game thread would need
    // the flag set under the type mutex or made atomic, or a cross-thread drain could keep a
    // mid-install mirror past disconnect.
    size_t DrainMirrorsOnly() {
        std::vector<std::unique_ptr<T>> drained;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (auto it = m_byId.begin(); it != m_byId.end();) {
                if (it->second && it->second->IsMirror()) {
                    drained.push_back(std::move(it->second));
                    it = m_byId.erase(it);
                } else {
                    ++it;
                }
            }
        }
        return drained.size();
        // The destructors fire here, outside the mutex.
    }

    // Drain only the wire mirrors owned by `ownerSlot`: when a peer drops, its mirrored props
    // become orphan ids that must vacate the Registry, so a reconnecting peer or a recycled id does
    // not collide. The same lock discipline and the same game-thread contract as DrainMirrorsOnly.
    size_t DrainMirrorsForSlot(int ownerSlot) {
        const int8_t want = static_cast<int8_t>(ownerSlot);
        std::vector<std::unique_ptr<T>> drained;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (auto it = m_byId.begin(); it != m_byId.end();) {
                if (it->second && it->second->IsMirror() &&
                    it->second->GetOwnerSlot() == want) {
                    drained.push_back(std::move(it->second));
                    it = m_byId.erase(it);
                } else {
                    ++it;
                }
            }
        }
        return drained.size();
        // The destructors fire here, outside the mutex.
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_byId.size();
    }

private:
    MirrorManager() = default;
    ~MirrorManager() = default;

    mutable std::mutex m_mutex;
    std::unordered_map<ElementId, std::unique_ptr<T>> m_byId;
};

}  // namespace coop::element
