// coop/prop_element_tracker.cpp -- see coop/prop_element_tracker.h.
//
// Extracted from prop_lifecycle.cpp 2026-05-29 (M-1 follow-up). Behavior
// preserved byte-for-byte from the original: same mutex scopes, same
// memory orders, same idempotency / cap / overflow-log semantics, same
// in-lock role read for the host/peer allocation-range decision.

#include "coop/prop_element_tracker.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::prop_element_tracker {
namespace {

namespace R = ue_wrap::reflection;

// ---- Cached session pointer (mirrors prop_lifecycle's; see header) ------
std::atomic<coop::net::Session*> g_session_ptr{nullptr};

inline coop::net::Session* LoadSession() {
    return g_session_ptr.load(std::memory_order_acquire);
}

// ---- ProcessedInit dedupe set -------------------------------------------
// Steady-state size = number of live keyed-interactable actors (entries
// added on Init POST, removed on K2_DestroyActor PRE). VOTV worlds have
// ~2000 live keyed-interactables on load. Cap at 16384 is a safety
// backstop against unbounded growth from a future bug; on overflow we LOG
// and stop inserting (do NOT clear -- clearing would un-dedupe the next
// super-call for every already-tracked actor and silently double-broadcast
// PropSpawn).
//
// The Init POST + K2_DestroyActor PRE observers can dispatch on a
// parallel-anim worker thread per ue_wrap/game_thread.h:118-120.
// Concurrent insert/erase/count on the unordered_set is UB (rehash
// invalidates iterators). Mutex held only for the brief set op; no
// engine calls under lock.
std::mutex g_processedInitMutex;
std::unordered_set<void*> g_processedInitActors;
constexpr size_t kProcessedInitCap = 16384;
std::atomic<bool> g_processedInitOverflowLogged{false};

// ---- KnownKeyedProps maintained set -------------------------------------
// H2-redux 2026-05-28: maintained set of live keyed-interactable actors.
// Seeded ONCE at Install() via a single GUObjectArray walk; thereafter
// maintained by Init POST (insert) + K2_DestroyActor PRE (evict).
// Engine-state tracking, NOT session-state: not cleared on OnDisconnect
// (actors persist across coop session boundaries).
//
// MTA precedent: MTA's per-type managers (CClientPedManager etc) keep
// a live std::list maintained by ctor/dtor. We approximate with a
// global set keyed by actor pointer until we adopt the broader
// CClientElement shape.
//
// Cap mirrors g_processedInitActors (16384). VOTV worlds carry ~2000
// keyed interactables; the cap is a backstop against unbounded growth
// from a future bug.
std::mutex g_knownKeyedPropsMutex;
std::unordered_set<void*> g_knownKeyedProps;
constexpr size_t kKnownKeyedPropsCap = 16384;
std::atomic<bool> g_knownKeyedPropsOverflowLogged{false};

// ---- Prop Element shadow (PR-FOUNDATION-3 Inc3, 2026-05-30) --------------
// Each entry in g_knownKeyedProps has a parallel Prop Element. The SOLE
// canonical OWNER of every Prop Element -- this peer's own keyed-interactable
// locals AND remote_prop's wire mirrors -- is now the shared singleton
// coop::element::MirrorManager<Prop>::Instance() (PropMirrors()). The host/
// seed side here used to keep a bespoke g_propElementsById owner map; that
// duplicate owner is retired (RULE 2). The local Prop Elements are now
// AllocAndInstall'd into the SAME manager remote_prop already used for
// mirrors.
//
// Unlike Npc (host XOR client -> pure manager), a Prop manager legitimately
// MIXES local (m_mirror=false, AllocAndInstall here) and mirror (m_mirror=true,
// remote_prop::RegisterPropMirror Install) Elements on the same peer. That is
// correct because the IsMirror() flag is per-element (each dtor self-routes
// FreeId vs UnregisterMirror, and the disconnect drain selects mirrors only --
// see DrainMirrorsOnly). See mirror_manager.h class-doc EXCEPTION.
//
// We keep ONE bespoke map -- g_actorToPropElementId (actor* -> local eid) --
// the reverse lookup the K2_DestroyActor PRE gate + the Init-POST broadcast
// elementId stamp need. It is host/seed-side bookkeeping (not Element
// identity), so it stays here, mirroring npc_sync's g_actorToNpcId. Its mutex
// is a LEAF: every path releases it BEFORE calling PropMirrors().AllocAndInstall
// /Take (type mutex) or destructing a Prop (Registry mutex via FreeId), so it
// never nests with either -> ABBA-free by construction.
//
// We DON'T preserve a thread_local PRE->POST handoff because, unlike NPCs,
// the Prop Element is created at Init POST time (engine already has the
// actor) and the actor pointer is bound directly inside MarkPropElement.
// No params-correlation gymnastics needed.
inline coop::element::MirrorManager<coop::element::Prop>& PropMirrors() {
    return coop::element::MirrorManager<coop::element::Prop>::Instance();
}

std::mutex g_actorToPropElementIdMutex;
std::unordered_map<void*, coop::element::ElementId> g_actorToPropElementId;

}  // namespace

// ---- Session pointer setter ---------------------------------------------

void SetSession(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);
}

// ---- ProcessedInit accessors --------------------------------------------

void MarkProcessedInit(void* actor) {
    if (!actor) return;
    std::lock_guard<std::mutex> lk(g_processedInitMutex);
    if (g_processedInitActors.size() >= kProcessedInitCap) {
        if (!g_processedInitOverflowLogged.exchange(true)) {
            UE_LOGW("prop_element_tracker: g_processedInitActors hit %zu cap; stopping inserts (Destroy-pruned in steady state -- if this fires we have an Init/Destroy imbalance)",
                    kProcessedInitCap);
        }
        return;
    }
    g_processedInitActors.insert(actor);
}

bool HasProcessedInit(void* actor) {
    if (!actor) return false;
    std::lock_guard<std::mutex> lk(g_processedInitMutex);
    return g_processedInitActors.count(actor) > 0;
}

void UnmarkProcessedInit(void* actor) {
    if (!actor) return;
    std::lock_guard<std::mutex> lk(g_processedInitMutex);
    g_processedInitActors.erase(actor);
}

size_t ClearProcessedInit() {
    std::lock_guard<std::mutex> lk(g_processedInitMutex);
    const size_t n = g_processedInitActors.size();
    g_processedInitActors.clear();
    g_processedInitOverflowLogged.store(false);
    return n;
}

// ---- KnownKeyedProps maintenance ----------------------------------------

void MarkKnownKeyedProp(void* actor) {
    if (!actor) return;
    std::lock_guard<std::mutex> lk(g_knownKeyedPropsMutex);
    if (g_knownKeyedProps.size() >= kKnownKeyedPropsCap) {
        if (!g_knownKeyedPropsOverflowLogged.exchange(true)) {
            UE_LOGW("prop_element_tracker: g_knownKeyedProps hit %zu cap; stopping inserts (snapshot will under-report -- Init/Destroy imbalance bug?)",
                    kKnownKeyedPropsCap);
        }
        return;
    }
    g_knownKeyedProps.insert(actor);
}

void UnmarkKnownKeyedProp(void* actor) {
    if (!actor) return;
    {
        std::lock_guard<std::mutex> lk(g_knownKeyedPropsMutex);
        g_knownKeyedProps.erase(actor);
    }
    // Resolve the local eid under the reverse-map mutex, erase the reverse
    // entry, then RELEASE the mutex before draining the Element out of the
    // shared manager. ABBA-safe: PropMirrors().Take takes the type mutex (not
    // nested with the leaf reverse-map mutex) and the drained unique_ptr
    // destructs OUTSIDE every lock -- ~Prop -> Registry::FreeId acquires the
    // Registry mutex with nothing else held.
    coop::element::ElementId eid = coop::element::kInvalidId;
    {
        std::lock_guard<std::mutex> lk(g_actorToPropElementIdMutex);
        auto it = g_actorToPropElementId.find(actor);
        if (it == g_actorToPropElementId.end()) return;
        eid = it->second;
        g_actorToPropElementId.erase(it);
    }
    auto drained = PropMirrors().Take(eid);
    // drained destructor fires here, OUTSIDE every mutex (FreeId, m_mirror=false).
    (void)drained;
}

// ---- Prop Element shadow ------------------------------------------------
//
// A2 (2026-05-29): role-aware allocation. HOST uses AllocHostId (host range
// [1, kHostRangeSize) -- authoritative side). CLIENT uses AllocLocalId
// (peer range [kHostRangeSize, kMaxElements) -- client-local elements).
// Without this split, both peers were popping from their independent host-
// range stacks for unrelated actors -- a Registry::Get(host_eid) on the
// client would resolve to the CLIENT's locally-allocated Prop, not the
// host's wire-broadcast intent. The mirror Elements created by the wire
// receivers (remote_prop::OnSpawn) now have a non-conflicting host-range
// slot to land in.
//
// Default-to-peer-range when no session pointer is set (boot/seed window):
// peer range is the safer default because the local peer's eventual role
// becomes irrelevant for these early elements -- if we later become host
// and broadcast PropSpawn for these seed elements, their elementId will be
// in peer range (clients route them as MIRROR entries which is correct).
//
// TOCTOU safety (audit fix 2026-05-28, retained through Inc3 2026-05-30): the
// idempotency check and the commit both hold g_actorToPropElementIdMutex, but
// the alloc step (PropMirrors().AllocAndInstall -> Registry::Alloc{Host,Local}Id
// + the manager's type-mutex emplace) runs with that leaf mutex RELEASED to
// keep the lock order leaf -> {type, Registry} non-nested. The double-check on
// re-acquire handles the race: if a concurrent thread committed the same actor
// while we were inside the manager, we drop ours -- Take it back out of the
// manager + let the destructor FreeId our just-allocated eid.
void MarkPropElement(void* actor, const std::wstring& key, const std::wstring& cls) {
    if (!actor) return;
    // Audit fix 2026-05-29: capture role INSIDE the same locked block as the
    // idempotency check. Reading role after the lock release would race
    // SetSession / role-change between the two reads -- a seed-time
    // MarkPropElement could observe role=Client at the check, allocate
    // peer-range, then become Host by the time the broadcast fires, and
    // ship a peer-range eid as a host-authoritative broadcast (slot-
    // collision risk on receivers). g_actorToPropElementIdMutex is the
    // synchronization point that owns this commit decision.
    bool isHost = false;
    {
        std::lock_guard<std::mutex> lk(g_actorToPropElementIdMutex);
        if (g_actorToPropElementId.count(actor) > 0) return;  // idempotent
        auto* s = LoadSession();
        isHost = (s != nullptr && s->role() == coop::net::Role::Host);
    }
    auto el = std::make_unique<coop::element::Prop>();
    auto toStr = [](const std::wstring& w) {
        std::string s; s.reserve(w.size());
        for (wchar_t c : w) s.push_back(static_cast<char>(c & 0xFF));
        return s;
    };
    if (!key.empty()) el->SetName(toStr(key));
    if (!cls.empty()) el->SetTypeName(toStr(cls));
    // Capture the GUObjectArray index now, while `actor` is live (Init POST /
    // discovery), so the late-joiner snapshot can later validate this pointer
    // via IsLiveByIndex without dereferencing it after a GC purge.
    el->SetActor(actor, R::InternalIndexOf(actor));
    // PR-FOUNDATION-3 Inc3: AllocAndInstall into the canonical PropMirrors()
    // (host range when host, peer range when client; m_mirror stays false).
    // Replaces the prior AllocHostId/AllocLocalId + bespoke g_propElementsById
    // emplace -- ONE owner now. On failure `el` is consumed/dropped inside.
    const coop::element::ElementId eid =
        PropMirrors().AllocAndInstall(std::move(el), isHost);
    if (eid == coop::element::kInvalidId) {
        UE_LOGW("prop_element_tracker: PropMirrors().AllocAndInstall returned kInvalidId "
                "for actor=%p key='%ls' -- Prop Element not registered "
                "(Registry exhausted / lifetime bug?)",
                actor, key.c_str());
        return;
    }
    // Re-check after the alloc window (the leaf mutex was released across
    // AllocAndInstall). If a concurrent MarkPropElement committed the same
    // actor meanwhile, drop ours.
    std::unique_lock<std::mutex> lk(g_actorToPropElementIdMutex);
    if (g_actorToPropElementId.count(actor) > 0) {
        // Lost the race. Release the leaf mutex, then Take our just-installed
        // Element back out of the manager and let it destruct -- ~Prop ->
        // Registry::FreeId returns `eid` to its origin stack and clears
        // m_byId[eid]. ABBA-safe: Take's type mutex + the dtor's Registry
        // mutex run with the leaf mutex released. (eid is in [1, kMaxElements)
        // -- AllocHostId/AllocLocalId never return 0 -- so Take won't treat it
        // as the wire-sentinel and no-op.)
        lk.unlock();
        auto losing = PropMirrors().Take(eid);
        (void)losing;  // destructs here, FreeId without any lock held
        return;
    }
    g_actorToPropElementId[actor] = eid;
}

coop::element::ElementId GetPropElementIdForActor(void* actor) {
    if (!actor) return coop::element::kInvalidId;
    std::lock_guard<std::mutex> lk(g_actorToPropElementIdMutex);
    auto it = g_actorToPropElementId.find(actor);
    if (it == g_actorToPropElementId.end()) return coop::element::kInvalidId;
    return it->second;
}

// ---- One-shot seed scan -------------------------------------------------
//
// Two-phase to avoid holding the mutex for the full ~150k GUObjectArray
// walk: phase 1 builds a local vector of live keyed-interactable pointers
// without any lock (reflection probes are thread-safe in our setup);
// phase 2 takes the mutex once and bulk-inserts.
void SeedKnownKeyedProps() {
    static std::atomic<bool> done{false};
    if (done.load(std::memory_order_acquire)) return;
    const int32_t n = R::NumObjects();
    int cdo = 0, dying = 0;
    std::vector<void*> live;
    live.reserve(4096);
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!ue_wrap::prop::IsKeyedInteractable(obj)) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) { ++cdo; continue; }
        if (!R::IsLive(obj)) { ++dying; continue; }
        live.push_back(obj);
    }
    int seeded = 0;
    {
        std::lock_guard<std::mutex> lk(g_knownKeyedPropsMutex);
        for (void* obj : live) {
            if (g_knownKeyedProps.size() >= kKnownKeyedPropsCap) break;
            g_knownKeyedProps.insert(obj);
            ++seeded;
        }
    }
    // Tier 3 Props migration 2026-05-28: also create Prop Element shadows
    // for each seeded actor so Registry::SnapshotByType<Prop> works as the
    // unified late-joiner snapshot path. Class + key resolved per-actor;
    // skip MarkPropElement on actors whose key is empty/None.
    //
    // Audit fix 2026-05-29: re-check IsLive at the start of phase 2. Phase 1
    // built `live` without holding any lock; an actor's K2_DestroyActor PRE
    // observer can fire between phases -- if MarkPropElement then commits a
    // dangling actor pointer into the shared PropMirrors() manager + the
    // g_actorToPropElementId reverse map, the eid leaks for the session
    // lifetime because UnmarkKnownKeyedProp already ran for that actor and
    // won't fire again.
    for (void* obj : live) {
        if (!R::IsLive(obj)) continue;
        const std::wstring cls = R::ClassNameOf(obj);
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(obj);
        if (key.empty() || key == L"None") continue;
        MarkPropElement(obj, key, cls);
    }
    UE_LOGI("prop_element_tracker: seeded known-keyed-props set with %d live actors (%d CDOs, %d dying skipped) -- subsequent snapshots skip GUObjectArray walk",
            seeded, cdo, dying);
    done.store(true, std::memory_order_release);
}

}  // namespace coop::prop_element_tracker
