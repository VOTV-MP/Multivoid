// coop/element/mirror_manager.h -- generic per-type mirror Element manager.
//
// Adapted from MTA's `CClient*Manager` family (CClientPlayerManager,
// CClientPedManager, CClientVehicleManager etc. -- vendored at
// reference/mtasa-blue/Client/mods/deathmatch/logic/CClient*Manager.h):
// each entity TYPE has its own manager that owns the per-id mirror map,
// the lifecycle (RegisterMirror via element::Registry), and the
// iteration / drain API. MTA does this as one hand-coded class per
// type; we generalize via a template parameterized on the
// `Element` subclass (Player / Npc / Prop / future Door / Vehicle).
//
// The 5-step RegisterMirror pattern (per [[feedback-registry-register-
// mirror-pattern]]) is encapsulated here so every per-type manager
// inherits the same ABBA-safe ordering automatically:
//   1. make_unique<T> by caller; passed to Install().
//   2. emplace into the owner map FIRST under m_mutex.
//   3. element::Registry::RegisterMirror SECOND (outside m_mutex acquire
//      avoids ABBA with the global registry lock).
//   4. On RegisterMirror failure: drain from the owner map (still under
//      m_mutex briefly) so the rolled-back unique_ptr destructs OUTSIDE
//      the lock; the dtor's ~Element early-returns because m_id stayed
//      kInvalidId.
//   5. Teardown via Drop() or DrainAll(): swap-out the unique_ptr under
//      m_mutex, release the lock, let the dtor fire outside the lock so
//      Registry::UnregisterMirror runs without holding the type-mutex.
//
// Why a template: the per-type managers share 100% of the lifecycle code
// (only the stored T type changes). MTA's hand-coded duplication is C++03
// constraints + Lua bindings that varied per type; we have neither, so
// the template form fits.
//
// Game thread vs net thread: Install / Drop / Take / Get / Snapshot /
// DrainAll all take the per-instance mutex; they're safe from any
// thread. The dtor work always runs OUTSIDE the mutex (Step 4 + Step 5
// above). The wrapped element::Registry takes its OWN mutex briefly
// inside RegisterMirror / UnregisterMirror; the ordering is type-mutex
// -> registry-mutex, never the other way -- the dtor pattern enforces it.
//
// Iteration: use Snapshot to copy raw pointers under lock + iterate
// without lock. There is intentionally NO ForEach(callback) -- running
// a callback under the mutex would invite ABBA against the registry
// mutex if the callback touches Element internals.
//
// Lifetime: each per-type manager is a process-lifetime singleton via
// `static MirrorManager<T>& Instance()`. The singleton is constructed
// at first call (C++17 thread-safe static init). On Stop the harness
// calls DrainAll() to release the mirrors before the session goes away.

#pragma once

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "ue_wrap/log.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

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

    // Register a mirror at `wireEid`. Returns true on success; false on
    // 0-eid / kInvalidId-eid, duplicate (caller's `mirror` is discarded
    // outside the lock so the dtor early-returns), or
    // element::Registry::RegisterMirror failure (rolled back; same
    // dtor-outside-lock pattern). Logs at each failure case.
    bool Install(ElementId wireEid, std::unique_ptr<T> mirror) {
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
        T* raw = nullptr;
        // Step 2 + duplicate-check under the per-type mutex. The mutex
        // is released BEFORE the registry call (Step 3) to keep the
        // ordering type-mutex -> registry-mutex strict.
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_byId.count(wireEid) > 0) {
                // Idempotent: re-spawn convergence packet for an eid we
                // already mirror. The local `mirror` falls out of scope
                // when this function returns; dtor sees m_id==kInvalidId
                // and early-returns without touching element::Registry.
                return false;
            }
            auto [it, ok] = m_byId.emplace(wireEid, std::move(mirror));
            if (!ok) return false;
            raw = it->second.get();
        }
        // Step 3: register with the global element::Registry, no
        // type-mutex held.
        if (!Registry::Get().RegisterMirror(wireEid, raw)) {
            UE_LOGE("MirrorManager: Registry::RegisterMirror(eid=0x%08x) FAILED -- "
                    "draining owner map (slot collision?)",
                    wireEid);
            // Step 4: drain the just-inserted entry. The losing
            // unique_ptr must destruct OUTSIDE m_mutex so the dtor can
            // safely take the registry mutex if it needs to (it
            // shouldn't here -- RegisterMirror failure means m_id is
            // still kInvalidId -- but the pattern is unconditional).
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

    // AUTHORITATIVE-ROLE ownership path (vs Install, the mirror-role path).
    // Allocates a FRESH ElementId from the Registry -- host range when
    // `isHost`, peer range otherwise -- binds `element` to it, and emplaces
    // under the type mutex. Returns the allocated id, or kInvalidId on null
    // element / Registry exhaustion / (shouldn't-happen) id collision; in
    // every failure case `element` is dropped here so the caller's unique_ptr
    // is consumed.
    //
    // Use this where the LOCAL peer is the authority that mints the id
    // (host-side NPC/Prop spawns; client-local props), as opposed to Install,
    // which binds an id ALREADY minted by a remote authority (a wire mirror).
    // The element's m_mirror stays false (AllocHostId/AllocLocalId don't set
    // it), so its dtor routes to Registry::FreeId -- returning the id to the
    // free stack -- NOT UnregisterMirror. Install'd mirrors set m_mirror=true
    // and route to UnregisterMirror. That single flag lets a unified drain
    // (DrainAll) release both kinds correctly.
    //
    // DEADLOCK SAFETY: this path takes the Registry mutex (inside AllocHostId/
    // AllocLocalId) and the type mutex (the emplace block) but NEVER holds both
    // at once -- AllocHostId fully releases the Registry mutex before the emplace
    // block acquires the type mutex. Install is the mirror image (type mutex
    // released before RegisterMirror takes the Registry mutex), and every drain
    // path destructs unique_ptrs OUTSIDE the type mutex so ~Element's FreeId
    // never nests either. With ZERO lock nesting anywhere in the element layer,
    // classical ABBA is structurally impossible -- regardless of role. (So the
    // host-XOR-client property below is NOT a deadlock-safety mechanism.) Do NOT
    // add a path that holds the type mutex across a Registry call (or vice
    // versa); that would break this by-construction guarantee.
    //
    // The host-XOR-client property IS load-bearing, but for OWNERSHIP/ROUTING
    // correctness, not locks: because a process uses AllocAndInstall (m_mirror
    // stays false) XOR Install (m_mirror=true) for type T, the manager holds a
    // pure host-set or pure client-set, so the IsMirror() flag cleanly
    // discriminates the unified drain (DrainAll / DrainClientMirrors).
    //
    // EXCEPTION -- Prop legitimately MIXES (PR-FOUNDATION-3 Inc3 2026-05-30):
    // unlike Npc (host XOR client), every peer runs BOTH owner paths for Prop:
    // prop_element_tracker::MarkPropElement AllocAndInstall's the peer's own
    // keyed-interactable world props (m_mirror=false) AND remote_prop::
    // RegisterPropMirror Install's wire mirrors of the OTHER peer's props
    // (m_mirror=true). So MirrorManager<Prop> holds both kinds at once. This is
    // STILL correct because the IsMirror() flag is PER-ELEMENT: every dtor
    // routes itself (FreeId vs UnregisterMirror), and the selective drains gate
    // per element. What the mixed case forbids is a BULK DrainAll on disconnect
    // (it would destroy the persistent local Prop Elements that must survive a
    // reconnect-within-the-same-process, since the seed scan is one-shot per
    // process) -- so the Prop disconnect path uses DrainMirrorsOnly() below.
    ElementId AllocAndInstall(std::unique_ptr<T> element, bool isHost) {
        if (!element) return kInvalidId;
        const ElementId eid = isHost ? Registry::Get().AllocHostId(element.get())
                                     : Registry::Get().AllocLocalId(element.get());
        if (eid == kInvalidId) {
            // Registry exhausted. AllocHostId/AllocLocalId only set the
            // element's m_id on SUCCESS, so `element` (destructing at return)
            // has m_id==kInvalidId and its ~Element skips FreeId. No leak.
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
            // A freshly-popped free-stack id already present in the owner map
            // means free-stack corruption / a lifetime bug upstream. `element`
            // (still owned -- not moved on this branch) destructs at return
            // OUTSIDE m_mutex, so its ~Element FreeId(eid) takes the Registry
            // mutex ABBA-safe. Logged, never silently swallowed.
            UE_LOGE("MirrorManager: AllocAndInstall fresh eid=%u already in owner "
                    "map -- dropping new element (free-stack/lifetime bug upstream)",
                    eid);
            return kInvalidId;
        }
        return eid;
    }

    // Drop the mirror at `wireEid`. Drain-under-lock-then-destruct so
    // the dtor fires outside m_mutex. Idempotent on missing eid.
    void Drop(ElementId wireEid) {
        (void)Take(wireEid);  // discard the returned unique_ptr; dtor fires here
    }

    // Same as Drop but returns the unique_ptr so the caller can inspect
    // the Element (e.g. read its actor pointer) BEFORE the dtor fires.
    // The caller MUST either let the returned unique_ptr fall out of
    // scope (dtor fires Registry::UnregisterMirror, ABBA-safe because
    // the type-mutex is no longer held) OR re-Install it. Returns
    // null unique_ptr on missing eid.
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
        // Type-mutex released. Caller now owns `drained`.
        return drained;
    }

    // O(1) lookup. Returns nullptr if not present. Game thread or any
    // thread that doesn't need the pointer to outlive a concurrent
    // Drop (callers reading actor state should also re-validate via
    // R::IsLive after Get returns).
    T* Get(ElementId wireEid) {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_byId.find(wireEid);
        return (it == m_byId.end()) ? nullptr : it->second.get();
    }

    // Snapshot copy of all mirror raw pointers under lock. Caller
    // iterates without lock. Use this for any iteration that might do
    // engine work (UFunction call, K2_DestroyActor) -- holding m_mutex
    // across engine work invites ABBA against the registry mutex.
    void Snapshot(std::vector<T*>& out) const {
        out.clear();
        std::lock_guard<std::mutex> lk(m_mutex);
        out.reserve(m_byId.size());
        for (const auto& kv : m_byId) out.push_back(kv.second.get());
    }

    // Drain everything on disconnect. Returns count drained. The
    // unique_ptrs destruct after the lock releases.
    //
    // Use this only for a PURE manager (host XOR client -- e.g. Npc). For a
    // manager that MIXES AllocAndInstall'd locals with Install'd mirrors
    // (Prop -- see the class doc EXCEPTION), use DrainMirrorsOnly on disconnect
    // so the persistent locals survive; DrainAll there would wrongly destroy
    // them.
    size_t DrainAll() {
        std::unordered_map<ElementId, std::unique_ptr<T>> drained;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            drained.swap(m_byId);
        }
        return drained.size();
        // dtors fire here, OUTSIDE m_mutex.
    }

    // Drain ONLY the wire-mirror entries (IsMirror() == true), leaving the
    // AllocAndInstall'd locals (m_mirror == false) in place. Returns count
    // drained. Selective disconnect drain for a MIXED manager (Prop): the
    // wire mirrors are session-state (they bind to the remote authority's
    // elementId and must vacate on disconnect), while the locals are
    // engine-state -- shadows of this peer's own persistent world props that
    // must survive a reconnect (the keyed-prop seed scan is one-shot per
    // process, so a wiped local Prop Element is never re-created).
    //
    // ABBA-safe: the drained unique_ptrs are moved out under m_mutex but
    // destruct OUTSIDE it (each ~Element -> Registry::UnregisterMirror takes
    // the Registry mutex).
    //
    // m_mirror VISIBILITY (review 2026-05-30): the IsMirror() read here is NOT
    // made safe by m_mutex. m_mirror is written by Install -> Registry::
    // RegisterMirror (SetMirror_) under the REGISTRY mutex, AFTER Install has
    // already released THIS manager's m_mutex -- a different lock, so the
    // manager mutex establishes no happens-before edge to that write. What
    // makes the read correct is GAME-THREAD SERIALIZATION: for the only mixed
    // type (Prop), the mirror producer (remote_prop::RegisterPropMirror ->
    // Install, driven by the wire receiver) and this drain (remote_prop::
    // ForceRelease) both run on the game thread within net_pump::Tick, so the
    // flag write program-orders before the drain read on the one thread.
    // AllocAndInstall'd LOCALS never touch m_mirror (it stays false from
    // construction, published by the emplace under m_mutex), so ONLY the mirror
    // case leans on that GT serialization. DO NOT move RegisterPropMirror /
    // Install off the game thread without first setting m_mirror under m_mutex
    // (or making it atomic): a cross-thread drain could then observe a
    // mid-Install mirror as m_mirror==false and wrongly KEEP it -> a wire mirror
    // leaked past disconnect with a dangling actor (plus a data race on the
    // non-atomic bool).
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
        // dtors fire here, OUTSIDE m_mutex.
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
