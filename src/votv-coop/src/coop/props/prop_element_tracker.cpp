// coop/prop_element_tracker.cpp -- see coop/prop_element_tracker.h.
//
// Extracted from prop_lifecycle.cpp 2026-05-29 (M-1 follow-up). Behavior
// preserved byte-for-byte from the original: same mutex scopes, same
// memory orders, same idempotency / cap / overflow-log semantics, same
// in-lock role read for the host/peer allocation-range decision.

#include "coop/props/prop_element_tracker.h"

#include "coop/dev/eid_lifetime_trace.h"  // Phase 1 S8.2: record the capture-eid (read-only host trace)
#include "coop/element/element_deleter.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"  // PropMirrors/NpcMirrors/WaMirrors
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/creatures/kerfur_entity.h"  // K-5: IsKerfurActor (the client-mint gate)
#include "coop/net/session.h"
#include "ue_wrap/engine.h"  // GetActorLocation (v86 Path 1c pile save-time map)
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::prop_element_tracker {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

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
// The actor* -> local eid reverse lookup the K2_DestroyActor PRE gate + the Init-POST
// broadcast elementId stamp need is now the UNIFIED Registry reverse (Registry::EidForActor,
// sync-refactor 2026-06-28). The old bespoke g_actorToPropElementId map (+ its leaf mutex) is
// RETIRED: it was a LOCALS-ONLY satellite of the registry; EidForActor covers locals AND
// mirrors in one owner, so GetPropElementIdForActor re-imposes the locals-only contract with an
// IsMirror filter. The two-concurrent-mint TOCTOU is resolved by MarkPropElement's post-alloc
// double-check on the reverse (the old mutex never prevented it either -- it released between
// the check and the commit).
//
// We DON'T preserve a thread_local PRE->POST handoff because, unlike NPCs,
// the Prop Element is created at Init POST time (engine already has the
// actor) and the actor pointer is bound directly inside MarkPropElement.
// No params-correlation gymnastics needed.
using coop::element::PropMirrors;   // canonical accessor (coop/element/mirror_managers.h)

// (sync-refactor 2026-06-27) The is-save-native question is now an Element field
// (Element::IsSaveNative), set by save_identity_bind at bind. The old
// g_boundMirrorNatives actor-keyed SET is RETIRED: it existed only because a
// mirror was absent from the locals-only g_actorToPropElementId reverse, and it
// could read STALE relative to the binding (the 15:01:49 D1 root). The unified
// Registry actor->eid reverse + the Element flag replace it -- see
// IsBoundMirrorNative below.

// ---- Key -> live-actor index (O(1) FindByKeyString replacement) ----------
// Maintained alongside g_actorToPropElementId: every keyed prop that commits a
// Prop Element (MarkPropElement) is indexed key -> {actor, internalIdx} here,
// and evicted on UnmarkKnownKeyedProp / ReapDeadLocalPropElements. This is THE
// fix for the connect-time re-snapshot balloon: remote_prop::OnSpawn used to
// de-dupe EACH of ~2316 re-snapshotted props via ue_wrap::prop::FindByKeyString,
// a full ~150k-object GUObjectArray walk WITH a per-candidate wstring alloc +
// GetKey UFunction dispatch -- O(N_props x N_objects) -> ~5.3M wstring allocs ->
// the client RSS ballooned to multi-GB and the session stalled. With this index
// each de-dupe is a single hash lookup + one IsLiveByIndex.
//
// Bidirectional: g_keyToActor for the lookup, g_actorToKey for actor-keyed
// removal (Unmark/Reap have the actor, not the key). The forward entry caches
// the GUObjectArray InternalIndex so ResolveLiveActorByKey can validate liveness
// via IsLiveByIndex WITHOUT dereferencing a possibly-freed actor pointer (the
// [[feedback-islive-unsafe-on-freed-cached-pointer]] rule).
//
// Keys are NOT globally unique across a level reload (a purged prop's Key can
// reappear on a fresh actor) and an actor address can be GC-recycled. Insert
// OVERWRITES (newest live actor wins). A stale forward entry that survives an
// address recycle (old key still pointing at a since-reused address+old index)
// is HARMLESS: IsLiveByIndex rejects it on lookup (the recycled slot no longer
// matches the old index), and ResolveLiveActorByKey lazily evicts it. Removal
// is by the actor's CURRENT key mapping so it never clobbers a newer prop that
// recycled the same address.
//
// g_keyIndexMutex is a LEAF: every path acquires it alone (no engine calls, no
// other mutex held) and releases it before any IsLiveByIndex / FindByKeyString /
// Registry / ElementDeleter call -> ABBA-free with all sibling mutexes.
struct KeyActorEntry {
    void*   actor       = nullptr;
    int32_t internalIdx = -1;
};
std::mutex g_keyIndexMutex;
std::unordered_map<std::wstring, KeyActorEntry> g_keyToActor;
std::unordered_map<void*, std::wstring> g_actorToKey;

// Insert / refresh the key index for `actor` (forward + reverse). No-op for
// empty/None keys (non-syncable props are never looked up by key). If `actor`
// previously carried a DIFFERENT key (a rekey), the old forward entry is dropped
// first so it can't linger pointing at the live actor under a dead key.
// Caller must hold NO other mutex.
void IndexKeyForActor_(void* actor, const std::wstring& key, int32_t internalIdx) {
    if (!actor || key.empty() || key == L"None") return;
    std::lock_guard<std::mutex> lk(g_keyIndexMutex);
    auto ait = g_actorToKey.find(actor);
    if (ait != g_actorToKey.end() && ait->second != key) {
        auto oldit = g_keyToActor.find(ait->second);
        if (oldit != g_keyToActor.end() && oldit->second.actor == actor) {
            g_keyToActor.erase(oldit);
        }
    }
    g_keyToActor[key] = KeyActorEntry{actor, internalIdx};
    g_actorToKey[actor] = key;
}

// Remove the key index entries for `actor` (both directions). Erases the forward
// entry only if it still points at THIS actor (so an address-recycle by a newer
// prop, which overwrote g_actorToKey[actor], is not disturbed). Caller must hold
// NO other mutex.
void EraseKeyIndexForActor_(void* actor) {
    if (!actor) return;
    std::lock_guard<std::mutex> lk(g_keyIndexMutex);
    auto ait = g_actorToKey.find(actor);
    if (ait == g_actorToKey.end()) return;
    auto kit = g_keyToActor.find(ait->second);
    if (kit != g_keyToActor.end() && kit->second.actor == actor) {
        g_keyToActor.erase(kit);
    }
    g_actorToKey.erase(ait);
}

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
    // (sync-refactor 2026-06-27) The is-save-native flag now lives on the Element and dies with it (the mirror
    // lifecycle is owned by the MirrorManager); no separate set to clear here.
    // Evict the key->actor index entry (only if it still points at THIS actor --
    // an address-recycle that re-indexed a newer prop is left intact). Done
    // first, under its own leaf mutex with nothing else held.
    EraseKeyIndexForActor_(actor);
    // Resolve the local eid under the reverse-map mutex, erase the reverse
    // entry, then RELEASE the mutex before extracting the Element. This observer
    // (K2_DestroyActor PRE) can fire on a parallel-anim WORKER thread, so the
    // Element must NOT be destructed inline here -- a worker-instant ~Prop ->
    // Registry::FreeId would race any in-flight raw pointer to it. PR-FOUNDATION-3
    // Inc4 (2026-05-30): route the taken Element through ElementDeleter (exactly
    // the npc_sync NpcDestroy_PRE pattern) so the actual ~Prop/FreeId runs at the
    // single game-thread Flush (net_pump::Tick), being-deleted-flagged from the
    // moment it is queued. ABBA-safe: the leaf reverse-map mutex is released
    // before Take (type mutex) + Enqueue (deleter leaf mutex); neither nests.
    // Local-eid safety: the deferred eid stays allocated (in Registry m_byId, NOT
    // on the free stack) until Flush, so a concurrent MarkPropElement's
    // AllocAndInstall cannot re-pop it -- no eid-reuse collision (unlike a wire
    // mirror's remote-controlled eid, which is why the game-thread mirror destroy
    // paths stay inline; see remote_prop::UnregisterPropMirror).
    // Resolve the local eid via the unified Registry reverse (sync-refactor 2026-06-28;
    // g_actorToPropElementId retired). LOCALS-ONLY: a mirror actor is not ours to drain here.
    // The reverse map entry is cleared by ~Element at the deferred Flush (not erased here) --
    // and the IsBeingDeleted flag (set the instant the first Unmark Enqueued this Element)
    // gates a concurrent second Unmark on the same actor, replacing the old erase-once guard.
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(actor);
    if (eid == coop::element::kInvalidId) return;
    coop::element::Element* el = coop::element::Registry::Get().Get(eid);
    if (!el || el->IsMirror() || el->IsBeingDeleted()) return;
    if (auto taken = PropMirrors().Take(eid))
        coop::element::ElementDeleter::Get().Enqueue(std::move(taken));
}

// ---- is-save-native (sync-refactor 2026-06-27, was the bound-mirror guard) ----
// True iff `actor` is a SAVE-LOADED NATIVE bound as a host-range mirror (the
// Element::IsSaveNative flag, set by save_identity_bind at bind) AND still the
// live occupant of its slot. Sourced from the unified Registry reverse + the
// Element field instead of the retired g_boundMirrorNatives set, so it can never
// read stale relative to the binding (the D1 root). Self-heals: a recycled
// address whose Element no longer matches the live slot reads false.
bool IsBoundMirrorNative(void* actor) {
    if (!actor) return false;
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(actor);
    if (eid == coop::element::kInvalidId) return false;
    coop::element::Element* el = coop::element::Registry::Get().Get(eid);
    if (!el || !el->IsSaveNative()) return false;
    return R::IsLiveByIndex(actor, el->GetInternalIdx());  // recycled/dead -> false
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
// TOCTOU safety (sync-refactor 2026-06-28, was audit fix 2026-05-28): idempotency is
// now a Registry::EidForActor probe (the reverse is registry-mutex-guarded), and the
// commit IS the AllocAndInstall (it writes the reverse at id-assignment). The
// post-alloc double-check (EidForActor(actor) != our eid) handles the race: if a
// concurrent MarkPropElement minted the same actor and won the reverse, we drop ours --
// Take it back out of the manager + let the deferred destructor FreeId our eid. (The
// old g_actorToPropElementIdMutex never actually serialized the check-vs-commit -- it
// released across AllocAndInstall -- so the double-check was always the real guard.)
void MarkPropElement(void* actor, const std::wstring& key, const std::wstring& cls) {
    if (!actor) return;
    // Guard: never mint a LOCAL element on an actor already bound as a host-range save-native MIRROR
    // (save_identity_bind). Without this the post-load SeedWalk_ would re-localize every bound native (the
    // g_actorToPropElementId idempotency check below does not know mirrors). Now sourced from the Element
    // flag via IsBoundMirrorNative (sync-refactor 2026-06-27). No-op in normal play.
    if (IsBoundMirrorNative(actor)) return;
    // Audit fix 2026-05-29: capture role INSIDE the same locked block as the
    // idempotency check. Reading role after the lock release would race
    // SetSession / role-change between the two reads -- a seed-time
    // MarkPropElement could observe role=Client at the check, allocate
    // peer-range, then become Host by the time the broadcast fires, and
    // ship a peer-range eid as a host-authoritative broadcast (slot-
    // collision risk on receivers). g_actorToPropElementIdMutex is the
    // synchronization point that owns this commit decision.
    // K-5 client-mint gate: a CLIENT must NEVER mint a (peer-range) eid for a kerfur prop -- its
    // identity is host-managed (the host owns the KerfurId + the host-range eid; the client adopts the
    // host's mirror via KerfurConvert / the connect snapshot). Minting one is the grab-dupe root
    // (redesign Failures #1/#3/#6). The HOST still registers its kerfur props normally (they need a
    // host-range eid for the snapshot + the conversion poll), and BindFormActor lazily allocates the
    // KerfurId at the first conversion, so no host-side first-sighting is needed here. Read OUTSIDE the
    // lock (reflection-only, no mutex), gate INSIDE it on the same role read the alloc-range uses.
    const bool isKerfur = coop::kerfur_entity::IsKerfurActor(actor);
    // Idempotency via the unified Registry reverse (sync-refactor 2026-06-28; g_actorToPropElementId
    // retired). ANY existing binding for this actor -- local OR mirror -- means it is already tracked,
    // so never mint a second LOCAL on top. This is STRICTLY SAFER than the old locals-only count: it
    // also blocks minting a local over a live mirror actor (a latent dup the locals-only map couldn't
    // see). The post-alloc double-check below resolves the two-concurrent-mint TOCTOU (the old leaf
    // mutex never prevented that either -- it released between the check and the commit; the
    // double-check was always the real guard).
    if (coop::element::Registry::Get().EidForActor(actor) != coop::element::kInvalidId) return;
    auto* s = LoadSession();
    const bool isHost = (s != nullptr && s->role() == coop::net::Role::Host);
    if (isKerfur && s != nullptr && s->role() == coop::net::Role::Client) return;  // K-5: no client mint
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
    // via IsLiveByIndex without dereferencing it after a GC purge. Reused below
    // to seed the key->actor index with the same liveness anchor.
    const int32_t internalIdx = R::InternalIndexOf(actor);
    el->SetActor(actor, internalIdx);
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
    // Re-check after the alloc window. AllocAndInstall wrote the unified Registry reverse
    // (actor -> eid) at id-assignment. If a concurrent MarkPropElement also minted this actor
    // and its write WON the reverse (reverse != our eid), drop ours: extract our just-installed
    // Element + route it through ElementDeleter -- this MarkPropElement can run on a parallel-anim
    // WORKER thread (Init POST), so ~Prop -> Registry::FreeId must NOT run inline at this worker
    // instant. Deferred to the game-thread Flush, being-deleted-flagged on Enqueue. (eid is in
    // [1, kMaxElements) -- AllocHostId/AllocLocalId never return 0 -- so Take won't no-op on the
    // wire-sentinel.) Exactly ONE of the two racers' reverse writes wins, so exactly one survives.
    if (coop::element::Registry::Get().EidForActor(actor) != eid) {
        if (auto taken = PropMirrors().Take(eid))
            coop::element::ElementDeleter::Get().Enqueue(std::move(taken));
        return;
    }
    // Index key -> live actor for the O(1) wire-key de-dupe used by the connect re-snapshot
    // (remote_prop::OnSpawn). Done AFTER the commit + only on the winning path (the lost-race
    // path above returns without indexing -> the key index + the registry reverse stay consistent).
    IndexKeyForActor_(actor, key, internalIdx);
}

coop::element::ElementId GetPropElementIdForActor(void* actor) {
    if (!actor) return coop::element::kInvalidId;
    // Sourced from the unified Registry reverse (sync-refactor 2026-06-28; the bespoke
    // g_actorToPropElementId map is RETIRED). The reverse covers BOTH locals and mirrors,
    // so re-impose this accessor's LOCALS-ONLY contract: a mirror actor resolves to
    // kInvalidId here (the K2_DestroyActor PRE gate + the Init-POST broadcast stamp act on
    // owned locals only; the mirror side has its own ResolveMirrorEidByActor).
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(actor);
    if (eid == coop::element::kInvalidId) return coop::element::kInvalidId;
    coop::element::Element* el = coop::element::Registry::Get().Get(eid);
    if (!el || el->IsMirror()) return coop::element::kInvalidId;
    return eid;
}

void RebindLocalElementActor(coop::element::ElementId eid, void* newActor) {
    // Game-thread only (all morph edges run on the game thread); the reverse map is mutex-guarded.
    if (eid == coop::element::kInvalidId || eid == 0u || !newActor) return;
    coop::element::Element* el = coop::element::Registry::Get().Get(eid);
    if (!el) return;
    if (el->IsMirror()) {
        // A mirror eid is the wrong path -- remote_prop::RegisterPropMirror(...,rebindInPlace)
        // owns mirror rebinds (and the reverse map below is host/seed-side local bookkeeping a
        // mirror was never in). Defensive guard; trash_channel only calls this for an isLocal eid.
        UE_LOGW("prop_element_tracker::RebindLocalElementActor: eid=%u is a MIRROR -- ignoring "
                "(use RegisterPropMirror rebindInPlace)", eid);
        return;
    }
    void* oldActor = el->GetActor();
    if (oldActor == newActor) return;  // idempotent
    // Re-point the canonical Element (+ cached liveness index) so ResolveLiveActorByEid(eid)
    // now resolves the new rendering of E. SetActor maintains the unified Registry reverse
    // (NoteActorRebind: drops oldActor->eid, sets newActor->eid) -- the bespoke
    // g_actorToPropElementId re-point is RETIRED (sync-refactor 2026-06-28).
    el->SetActor(newActor, R::InternalIndexOf(newActor));
    UE_LOGI("prop_element_tracker::RebindLocalElementActor: eid=%u re-pointed %p -> %p (morph re-skin)",
            eid, oldActor, newActor);
}

// ---- Key -> live-actor lookup (public) ----------------------------------

void IndexActorKey(void* actor, const std::wstring& key) {
    if (!actor || key.empty() || key == L"None") return;
    IndexKeyForActor_(actor, key, R::InternalIndexOf(actor));
}

void* FindLiveActorByKey(const std::wstring& key) {
    if (key.empty() || key == L"None") return nullptr;
    void* actor = nullptr;
    int32_t internalIdx = -1;
    {
        std::lock_guard<std::mutex> lk(g_keyIndexMutex);
        auto it = g_keyToActor.find(key);
        if (it == g_keyToActor.end()) return nullptr;
        actor = it->second.actor;
        internalIdx = it->second.internalIdx;
    }
    // Validate liveness WITHOUT dereferencing the cached pointer (it may have
    // been GC-freed since indexing). IsLiveByIndex reads only the GUObjectArray
    // slot at the cached index. A stale entry (slot recycled / index no longer
    // points back) reports not-live -> lazily evict it so a never-looked-up-again
    // recycle leak can't accumulate, then return nullptr (caller falls back to
    // the cold scan, which will also miss -> behaves exactly like pre-index).
    if (R::IsLiveByIndex(actor, internalIdx)) return actor;
    {
        std::lock_guard<std::mutex> lk(g_keyIndexMutex);
        auto it = g_keyToActor.find(key);
        if (it != g_keyToActor.end() && it->second.actor == actor &&
            it->second.internalIdx == internalIdx) {
            g_keyToActor.erase(it);
            g_actorToKey.erase(actor);
        }
    }
    return nullptr;
}

void* ResolveLiveActorByKey(const std::wstring& key, bool* outFellBackToScan) {
    if (outFellBackToScan) *outFellBackToScan = false;
    if (key.empty() || key == L"None") return nullptr;
    if (void* a = FindLiveActorByKey(key)) return a;  // O(1) maintained-index hit
    // Cold fallback: a prop that exists locally but isn't indexed yet. The
    // GUObjectArray scan preserves exact pre-index behavior; it runs only on an
    // index miss. A SUCCESSFUL fallback means the index was STALE -- a live prop
    // with this key exists but wasn't indexed (the index points at dead actors
    // after a world-change purge the slow re-seed hasn't caught up with). The
    // caller uses outFellBackToScan to trigger a throttled re-seed so the rest of
    // a snapshot de-dupe burst resolves O(1) instead of each paying this scan
    // (the [[project-bug-prop-resnapshot-leak]] balloon). A scan MISS (genuinely
    // absent prop) leaves the flag false -- nothing to re-seed, the caller spawns.
    void* scanned = ue_wrap::prop::FindByKeyString(key);
    if (scanned && outFellBackToScan) *outFellBackToScan = true;
    return scanned;
}

bool ReconcileIndexThrottled() {
    // Self-heal for a STALE key index detected mid-de-dupe (a world-change purged
    // the indexed actors and the steady-state re-seed -- gated on the slow 256/4s
    // reaper -- hasn't caught up). Drain the dead Prop Elements then re-seed so the
    // key index reflects the CURRENT loaded world; the in-flight snapshot burst
    // then de-dupes O(1). Throttled so only the FIRST stale fallback in a burst
    // pays the ~150k-object re-seed walk (the rest hit the freshly-rebuilt index).
    // Game-thread only (drain/re-seed are GT contracts; the de-dupe caller is GT).
    static std::mutex sThrottleMutex;
    static std::chrono::steady_clock::time_point sLast{};
    static bool sEver = false;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(sThrottleMutex);
        if (sEver && (now - sLast) < std::chrono::milliseconds(200)) return false;
        sLast = now;
        sEver = true;
    }
    size_t drained = 0;
    for (int pass = 0; pass < 8; ++pass) {
        const size_t r = ReapDeadLocalPropElements(4096);
        drained += r;
        if (r < 4096) break;  // backlog cleared
    }
    const size_t added = ReSeedKnownKeyedProps();
    UE_LOGI("prop_element_tracker: stale-index self-heal -- drained %zu dead, re-seeded %zu new keyed prop(s) into the key index (snapshot de-dupe now O(1))",
            drained, added);
    return true;
}

// ---- Seed / re-seed scan ------------------------------------------------
//
// Shared walk body for the one-shot boot seed AND an explicit re-seed (level/
// world change). Two-phase to avoid holding the mutex for the full ~150k
// GUObjectArray walk: phase 1 builds a local vector of live keyed-interactable
// pointers without any lock (reflection probes are thread-safe in our setup);
// phase 2 takes the mutex once and bulk-inserts. Both phases are IDEMPOTENT --
// g_knownKeyedProps.insert is a set insert (no-op if present) and MarkPropElement
// no-ops on an already-tracked actor -- so a re-seed only ADDS props the world
// gained that we are not yet tracking. Returns counts; `newlyTracked` is how many
// keyed actors this walk added to g_knownKeyedProps (= the whole set on the first
// boot seed; the delta on a re-seed).
namespace {
struct SeedCounts { int liveFound = 0; int newlyTracked = 0; int cdo = 0; int dying = 0; int keylessPiles = 0; };

// ---- World-coherence stamp (Fork A, 2026-06-10) --------------------------
// SeedWalk_ stamps the live gameplay UWorld it ran against; the snapshot
// gate (prop_snapshot::TriggerForSlot) refuses to open a bracket unless that
// world is still the live one. During a world transition the registry holds
// the dead world's props until the drain-complete re-seed -- a bracket built
// then is near-empty and the client's adoption sweep destroys against it
// (the 2026-06-10 1300-actor mass destroy). GT-write (seeds are GT
// contracts), GT-read (TriggerForSlot/DrainChunk); atomics for the file's
// sibling-symmetry only.
std::atomic<bool>     g_seededOnce{false};
std::atomic<void*>    g_seedWorld{nullptr};
std::atomic<int32_t>  g_seedWorldIdx{-1};
std::atomic<uint64_t> g_seedGeneration{0};
// Purge-episode flag (gate hardening, 2026-06-10 smoke falsification): the
// reaper detected a mass purge and the registry is draining dead elements
// until the episode-end re-seed. The world-stamp above is NOT sufficient on
// its own: VOTV's boot/save-load flow can leave the stamped UWorld ALIVE
// while the registry is majority-dead (smoke 15:16: stamp live, 1161 dying
// vs 88 live at enumerate -> the gate passed and the client swept 3067
// actors against an 88-prop bracket). net_pump owns the detection edges;
// this is registry-coherence state so the tracker owns the flag.
std::atomic<bool>     g_inPurgeEpisode{false};

SeedCounts SeedWalk_(std::vector<void*>* outNewActors) {
    const int32_t n = R::NumObjects();
    SeedCounts c;
    std::vector<void*> live;
    live.reserve(4096);
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!ue_wrap::prop::IsKeyedInteractable(obj)) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) { ++c.cdo; continue; }
        if (!R::IsLive(obj)) { ++c.dying; continue; }
        live.push_back(obj);
    }
    c.liveFound = static_cast<int>(live.size());
    {
        std::lock_guard<std::mutex> lk(g_knownKeyedPropsMutex);
        for (void* obj : live) {
            if (g_knownKeyedProps.size() >= kKnownKeyedPropsCap) break;
            if (g_knownKeyedProps.insert(obj).second) {
                // CHURN GUARD (2026-07-03, the 11:48:59 keyless-PropSpawn re-broadcast):
                // an actor ALREADY BOUND to a live owned element is NOT new -- it is the
                // churned actor of a tracked element (a host RE-PILE land re-creates the
                // pile actor in place and the pile layer rebinds the element same-tick).
                // The private known-set is a pointer set, blind to actor churn (the
                // re-bind thread's exact lesson: identity maps that don't track churn
                // smear) -- without this test every land result re-entered outNewActors
                // and net_pump re-broadcast it as an incremental keyless PropSpawn every
                // 20 s re-seed. The set insert above still refreshes membership (the
                // O(1) snapshot de-dupe), it just is not "newness" any more.
                if (GetPropElementIdForActor(obj) != coop::element::kInvalidId) continue;
                ++c.newlyTracked;
                // R1: yield the newly-adopted actor so the steady-world re-seed
                // can broadcast ONE incremental PropSpawn for it (the eid is
                // minted in phase 2 below, before this function returns, so a
                // caller iterating outNewActors resolves GetPropElementIdForActor).
                // Captured here (the ONLY newness signal -- phase-2 MarkPropElement
                // is idempotent + silent on already-tracked). May include keyless
                // non-pile actors that phase 2 won't express; the express filters them.
                if (outNewActors) outNewActors->push_back(obj);
            }
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
        if (key.empty() || key == L"None") {
            // Fork B HALF 1 (2026-06-10): keyless chipPile -- its cross-peer
            // identity is the ElementId (the v52 eid lane; precedent
            // trash_collect_sync::BroadcastConvertNear's idempotent
            // MarkPropElement). Minting an Element here puts the host's
            // world piles into the connect snapshot (the keyless skip in
            // DrainChunk routes them down the eidOnly receiver lane), which
            // is what lets the client adopt the host's pile set instead of
            // sweeping its own and receiving nothing. All OTHER keyless
            // actors (a held clump mid-flight, a pre-Init Aprop_C, a keyless
            // trashBits straggler) are NOT expressible and stay untracked --
            // symmetric with the sweep's universe test.
            if (ue_wrap::prop::IsChipPile(obj)) {
                MarkPropElement(obj, L"", cls);
                ++c.keylessPiles;
            }
            continue;
        }
        MarkPropElement(obj, key, cls);  // idempotent
    }
    // Stamp the gameplay world this walk expressed. MUST IsLive-filter
    // (mid-transition the DYING old world sits at a lower GUObjectArray
    // index) and gameplay-name-filter (matches the reaper's gate in
    // net_pump -- a menu/preLoad world never opens the snapshot gate).
    // Perf audit W-2 (2026-06-10): inline pointer-compare walk, NOT
    // FindObjectsByClass -- that helper allocates a ClassNameOf wstring per
    // GUObjectArray entry (~250k allocs), doubling every seed walk. The
    // UWorld UClass resolves once (sticky); per entry this walk is two
    // pointer reads, with ToString only on actual World instances (a
    // handful).
    {
        static std::atomic<void*> sWorldCls{nullptr};
        void* worldCls = sWorldCls.load(std::memory_order_acquire);
        if (!worldCls) {
            worldCls = R::FindClass(P::name::WorldClass);
            if (worldCls) sWorldCls.store(worldCls, std::memory_order_release);
        }
        void* w = nullptr;
        if (worldCls) {
            for (int32_t i = 0; i < n; ++i) {
                void* cand = R::ObjectAt(i);
                if (!cand || R::ClassOf(cand) != worldCls) continue;
                if (!R::IsLive(cand)) continue;
                if (R::ToString(R::NameOf(cand)).find(L"ntitled") == std::wstring::npos) continue;
                w = cand;
                break;
            }
        }
        if (w) {
            g_seedWorld.store(w, std::memory_order_release);
            g_seedWorldIdx.store(R::InternalIndexOf(w), std::memory_order_release);
        }
        // Bump on EVERY walk (even if no gameplay world resolved): every
        // coherence-restoring event is a generation bump, and the deferred-
        // slot flush in prop_snapshot keys on it.
        g_seedGeneration.fetch_add(1, std::memory_order_release);
    }
    return c;
}
}  // namespace

void SeedKnownKeyedProps() {
    // Latch promoted to the file-scope g_seededOnce (Fork A) so the snapshot
    // gate can refuse to bracket before the boot seed has ever run (RULE 2:
    // one latch, not a function-local twin).
    if (g_seededOnce.load(std::memory_order_acquire)) return;
    const SeedCounts c = SeedWalk_(nullptr);
    UE_LOGI("prop_element_tracker: seeded known-keyed-props set with %d live actors (%d new, %d keyless chipPile element(s), %d CDOs, %d dying skipped) -- subsequent snapshots skip GUObjectArray walk",
            c.liveFound, c.newlyTracked, c.keylessPiles, c.cdo, c.dying);
    g_seededOnce.store(true, std::memory_order_release);
}

size_t ReSeedKnownKeyedProps(std::vector<void*>* outNewActors) {
    const SeedCounts c = SeedWalk_(outNewActors);
    UE_LOGI("prop_element_tracker: re-seed found %d live keyed props, added %d NEW to tracking (%d keyless chipPile element(s), %d CDOs, %d dying) -- world/level-change reconcile [snapshot-completeness]",
            c.liveFound, c.newlyTracked, c.keylessPiles, c.cdo, c.dying);
    return static_cast<size_t>(c.newlyTracked);
}

void CollectTrackedKeyedPropKeys(std::unordered_set<std::wstring>& out) {
    // The key index (g_keyToActor) holds exactly the live keyed props that minted
    // a Prop Element with a non-empty wire-key -- i.e. the keyed Aprop_C set the
    // save persists. Keyless chipPiles never enter it (IndexKeyForActor_ no-ops
    // empty keys), which is exactly R2's scope (diff/delete by key). Leaf-mutex
    // copy; no engine calls under lock.
    std::lock_guard<std::mutex> lk(g_keyIndexMutex);
    out.reserve(out.size() + g_keyToActor.size());
    for (const auto& kv : g_keyToActor) out.insert(kv.first);
}

void CollectTrackedPileTransforms(
    std::unordered_map<coop::element::ElementId, ue_wrap::FVector>& out) {
    // v86 Path 1c: the host's save-time pile positions, keyed by host eid. Walk the
    // GUObjectArray for live keyless chipPiles; the position read HERE (right after the
    // scratch save was serialized -- save_transfer::OnRequest, same game-thread tick) is the
    // value the save holds (saveObjects read GetTransform the same instant), so it == what the
    // joining client loads its native at. KEYLESS chipPile only (a keyed Aprop_C is covered by
    // the R2 key-diff; only the keyless pile duped on a host-moved-in-window).
    //
    // SELF-SEED (take-4 ROOT FIX, 2026-06-23): a live chipPile with no eid here is simply
    // NOT-YET-RE-MINTED, not absent. The host's world-change re-seed can still be DEFERRED at
    // the connect instant -- net_pump parks it through the menu->game mass-purge drain
    // (net_pump.cpp:528), and because VOTV is ONE persistent UWorld the world-stamp stays LIVE,
    // so IsRegistrySeededForCurrentWorld() is TRUE and is no signal at all (the take-3 gate that
    // never fired). Empirically (take-2 + take-3 logs) this leaves EVERY pile eid==0 at capture
    // -> the eid-keyed map is EMPTY -> the client can't twin-destroy a host-moved-in-window pile
    // -> the join-window DUP. So MINT the eid right here (register-only, idempotent -- the SAME
    // path SeedWalk_ phase 2 uses at :618) instead of skipping. The eid is identical to the one
    // the connect drain later broadcasts (MarkPropElement no-ops on the now-tracked actor when
    // the deferred re-seed runs), so the client's save-time-position key resolves. The capture
    // now OWNS its precondition rather than trusting a seed-timing gate. One GUObjectArray walk,
    // cold connect-edge path, game thread (engine reads + register-only mints, no broadcast).
    const int32_t n = R::NumObjects();
    int minted = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!ue_wrap::prop::IsChipPile(obj)) continue;                 // lineage test, pure pointer walks
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // CDO
        coop::element::ElementId eid = GetPropElementIdForActor(obj);
        if (eid == coop::element::kInvalidId || eid == 0u) {
            MarkPropElement(obj, L"", R::ClassNameOf(obj));            // mint now (register-only, idempotent)
            eid = GetPropElementIdForActor(obj);                      // resolves in-call (minted before return)
            if (eid == coop::element::kInvalidId || eid == 0u)
                continue;  // mint declined (registry full) -> live-pose fallback for this pile
            ++minted;
        }
        out[eid] = ue_wrap::engine::GetActorLocation(obj);
        coop::dev::eid_lifetime_trace::RecordCaptureEid(obj, static_cast<uint32_t>(eid));  // S8.2 capture-eid
    }
    if (minted > 0)
        UE_LOGI("prop_element_tracker: CollectTrackedPileTransforms self-seeded %d unseeded live "
                "chipPile(s) (world-change re-seed still deferred at capture) -> %zu pile save-time xform(s)",
                minted, out.size());
}

void CollectTrackedKerfurTransforms(
    std::unordered_map<coop::element::ElementId, ue_wrap::FVector>& out) {
    // scope A (kerfur off->active dup retire, 2026-06-24): the host's save-time OFF-form-kerfur positions,
    // keyed by host eid. Identical mechanism to CollectTrackedPileTransforms (blob-instant capture +
    // self-seed) but gated to the kerfur prop lineage (prop_kerfurOmega_C + skins). A kerfur off-prop is
    // a KEYED Aprop (registered with its Aprop_Key by the normal seed), so the self-seed mints with the
    // real key, not the keyless chipPile placeholder. The host stamps this position onto the KerfurConvert
    // when it turns the kerfur ON in the join window; the joining client then RETIRES its stale local
    // off-prop matched at this exact key (the off-prop the host no longer expresses as off). One
    // GUObjectArray walk, game thread (engine reads + register-only mints, no broadcast).
    const int32_t n = R::NumObjects();
    int minted = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls || !coop::kerfur_entity::IsKerfurPropClass(cls)) continue;  // off-form kerfurs only
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;       // CDO
        coop::element::ElementId eid = GetPropElementIdForActor(obj);
        if (eid == coop::element::kInvalidId || eid == 0u) {
            const std::wstring key = ue_wrap::prop::GetInteractableKeyString(obj);  // kerfur off-prop is keyed
            MarkPropElement(obj, (key == L"None") ? std::wstring() : key, R::ClassNameOf(obj));
            eid = GetPropElementIdForActor(obj);  // resolves in-call (minted before return)
            if (eid == coop::element::kInvalidId || eid == 0u)
                continue;  // mint declined (registry full) -> no save-time key for this kerfur (live-pose fallback)
            ++minted;
        }
        out[eid] = ue_wrap::engine::GetActorLocation(obj);
        coop::dev::eid_lifetime_trace::RecordCaptureEid(obj, static_cast<uint32_t>(eid));  // S8.2 capture-eid
    }
    if (minted > 0)
        UE_LOGI("prop_element_tracker: CollectTrackedKerfurTransforms self-seeded %d unseeded live "
                "kerfur off-prop(s) -> %zu kerfur save-time xform(s)", minted, out.size());
}

bool HasSeededOnce() {
    return g_seededOnce.load(std::memory_order_acquire);
}

bool IsRegistrySeededForCurrentWorld() {
    // O(1): IsLiveByIndex reads ONLY the GUObjectArray slot metadata at the
    // captured index -- never the (possibly freed) world's memory. The
    // instant a world swap's GC purge kills the stamped UWorld, this reads
    // false with zero detection latency; the next SeedWalk_ (episode-end
    // re-seed / small-travel re-seed / self-heal) re-stamps the new world.
    void* w = g_seedWorld.load(std::memory_order_acquire);
    return w && R::IsLiveByIndex(w, g_seedWorldIdx.load(std::memory_order_acquire));
}

uint64_t SeedGeneration() {
    return g_seedGeneration.load(std::memory_order_acquire);
}

void SetInPurgeEpisode(bool active) {
    g_inPurgeEpisode.store(active, std::memory_order_release);
}

bool InPurgeEpisode() {
    return g_inPurgeEpisode.load(std::memory_order_acquire);
}

// ---- Dead-Element reaper ------------------------------------------------
//
// See header for WHY. Reaps by EID (not by actor pointer) with an IsMirror()
// gate so it is robust against the engine recycling a purged actor's address.

size_t ReapDeadLocalPropElements(size_t maxEvictions,
                                 std::vector<coop::element::ElementId>* outReapedEids) {
    if (maxEvictions == 0) return 0;
    // One mutex-guarded copy of (actor, eid, internalIdx, mirror) for every Prop
    // Element. No Element* deref after the Registry mutex releases; the cached
    // internalIdx lets us validate each actor via IsLiveByIndex WITHOUT
    // dereferencing a possibly-purged actor pointer (IsLive would AV).
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(
        coop::element::ElementType::Prop, pairs);
    size_t evicted = 0;
    for (const auto& pr : pairs) {
        if (evicted >= maxEvictions) break;
        if (pr.mirror) continue;                                   // wire mirror -> host's PropDestroy owns it
        if (R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;  // actor alive -> keep
        // Dead LOCAL Prop Element. Clear the actor-keyed bookkeeping ONLY where the
        // unified Registry reverse still maps pr.actor to THIS eid: if pr.actor's
        // address was recycled by a newer keyed prop, EidForActor(pr.actor) now names
        // the new eid and must NOT be disturbed (that would un-track the live new
        // Element). The reverse entry itself is cleared by ~Element at the deferred
        // Flush (NoteActorRebind, same ownership gate) -- no manual erase here
        // (sync-refactor 2026-06-28; g_actorToPropElementId retired).
        const bool ownActorEntry =
            (coop::element::Registry::Get().EidForActor(pr.actor) == pr.id);
        if (ownActorEntry) {
            {
                std::lock_guard<std::mutex> lk(g_knownKeyedPropsMutex);
                g_knownKeyedProps.erase(pr.actor);
            }
            UnmarkProcessedInit(pr.actor);
            // Evict the key->actor index too (same ownership gate -- erases the
            // forward entry only if it still points at this actor, so a recycled
            // address now indexing a newer prop is preserved). A rare stale entry
            // under a no-longer-used key is harmless (IsLiveByIndex rejects it on
            // lookup + ResolveLiveActorByKey lazily evicts it).
            EraseKeyIndexForActor_(pr.actor);
        }
        // Take the dead Element out of the canonical manager BY EID + defer its
        // destruct (FreeId) to the game-thread ElementDeleter::Flush -- identical
        // to UnmarkKnownKeyedProp's drain, minus the wire broadcast. Take(eid) is
        // a no-op returning null if a racing K2_DestroyActor PRE already took it
        // (Enqueue(null) is a no-op) -- single ownership transfer either way.
        auto taken = PropMirrors().Take(pr.id);
        // PART 1 death-watch yield: the HOST broadcasts an explicit PropDestroy(eid) for this
        // steady-state vanish the un-hookable BP path didn't replicate. SKIP kerfur props: a
        // kerfur's lifecycle is KerfurConvert, not PropDestroy -- the convert poll drains the dead
        // prop element ~200ms after a turn-on, and if the reaper races into that window a
        // PropDestroy here would conflict the KerfurConvert (client drops the kerfur, then re-
        // converts) -- the exact "spam turn-on/off births a dupe" class. The class string is the
        // reliable signal (the actor is already dead, so IsKerfurActor can't read it).
        if (outReapedEids && taken &&
            taken->GetTypeName().find("kerfurOmega") == std::string::npos) {
            outReapedEids->push_back(pr.id);
        }
        coop::element::ElementDeleter::Get().Enqueue(std::move(taken));
        ++evicted;
    }
    if (evicted > 0) {
        UE_LOGI("prop_element_tracker: reaped %zu dead local Prop Element(s) "
                "(mass-purge / level-transition cleanup; scanned %zu Prop Element(s), cap %zu/call)",
                evicted, pairs.size(), maxEvictions);
    }
    return evicted;
}

// ---- Reaper self-test ---------------------------------------------------

bool DebugCheckPropElementReap() {
    // Sentinel actor address that is NEVER dereferenced: IsLiveByIndex rejects it
    // on the internalIdx<0 fast-path (no GUObjectArray read), the maps key on the
    // pointer value only, and ~Prop does not own the actor -- so no real UObject
    // memory is ever touched by this fixture.
    void* deadActor = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD0001));
    auto* s = LoadSession();
    const bool isHost = (s != nullptr && s->role() == coop::net::Role::Host);

    // Install a synthetic DEAD local Prop Element wired EXACTLY as a real one is
    // (manager + all three actor-keyed maps), but with internalIdx = -1 so the
    // reaper's IsLiveByIndex gate classifies it dead -- standing in for an actor
    // the engine mass-purged without firing K2_DestroyActor.
    auto deadEl = std::make_unique<coop::element::Prop>();
    deadEl->SetName("synth-reap-dead");
    deadEl->SetActor(deadActor, -1);
    const coop::element::ElementId deadEid =
        PropMirrors().AllocAndInstall(std::move(deadEl), isHost);
    if (deadEid == coop::element::kInvalidId) {
        UE_LOGW("propreap_test: FAIL -- AllocAndInstall returned kInvalidId (Registry exhausted?)");
        return false;
    }
    // (AllocAndInstall already wrote the unified Registry reverse deadActor->deadEid at
    // id-assignment; the bespoke g_actorToPropElementId write is retired 2026-06-28.)
    MarkKnownKeyedProp(deadActor);
    MarkProcessedInit(deadActor);

    // LIVE control: a second synthetic LOCAL element bound to a REAL live UObject
    // (so IsLiveByIndex passes). The reaper MUST preserve it -- proving it never
    // false-positive-evicts a live prop. Pick the first live object that is NOT
    // already a tracked prop (so we never clobber real tracking). Using a non-prop
    // UObject as the actor is fine: the reaper only IsLiveByIndex-checks it (no
    // deref) and the maps key on the pointer; we clean it up below.
    void* liveActor = nullptr;
    int32_t liveIdx = -1;
    {
        const int32_t n = R::NumObjects();
        for (int32_t i = 0; i < n; ++i) {
            void* o = R::ObjectAt(i);
            if (o && R::IsLive(o) &&
                GetPropElementIdForActor(o) == coop::element::kInvalidId) {
                liveActor = o;
                liveIdx = R::InternalIndexOf(o);
                break;
            }
        }
    }
    coop::element::ElementId liveEid = coop::element::kInvalidId;
    if (liveActor) {
        auto liveEl = std::make_unique<coop::element::Prop>();
        liveEl->SetName("synth-reap-live");
        liveEl->SetActor(liveActor, liveIdx);
        liveEid = PropMirrors().AllocAndInstall(std::move(liveEl), isHost);
        // (AllocAndInstall wrote the registry reverse liveActor->liveEid; no bespoke map write.)
    }

    const bool preRegistered = (coop::element::Registry::Get().Get(deadEid) != nullptr) &&
                               (GetPropElementIdForActor(deadActor) == deadEid) &&
                               HasProcessedInit(deadActor);
    if (!preRegistered) {
        UE_LOGW("propreap_test: FAIL -- synthetic dead Element not fully registered pre-reap");
        return false;
    }

    const size_t reaped = ReapDeadLocalPropElements(256);

    // processed-init is cleared SYNCHRONOUSLY by the reaper. The unified Registry reverse
    // (deadActor->deadEid) is now owned by the Element lifecycle -- it clears at the deferred
    // ~Element Flush (checked below with eidFreed), not synchronously in the reaper
    // (sync-refactor 2026-06-28; g_actorToPropElementId retired).
    const bool initCleared = !HasProcessedInit(deadActor);
    // The LIVE control must SURVIVE (IsLiveByIndex true -> reaper skips it). Read
    // BEFORE the cleanup below. Vacuously true only if we found no live object to
    // bind (impossible in a live process; defensive).
    const bool livePreserved =
        (liveEid == coop::element::kInvalidId) ||
        (GetPropElementIdForActor(liveActor) == liveEid &&
         coop::element::Registry::Get().Get(liveEid) != nullptr);
    // The eid is freed only when the parked unique_ptr destructs -- force the
    // controlled game-thread Flush now (we ARE on the game thread) and re-check.
    coop::element::ElementDeleter::Get().Flush();
    // After the Flush the dead Element's dtor has run: its eid is freed AND the unified
    // Registry reverse entry (deadActor->deadEid) is cleared (NoteActorRebind in ~Element).
    const bool eidFreed = (coop::element::Registry::Get().Get(deadEid) == nullptr) &&
                          (GetPropElementIdForActor(deadActor) == coop::element::kInvalidId);

    // Clean up the live control so the test leaves no synthetic element bound to a
    // random live UObject in the real registry. (The registry reverse clears at ~Element;
    // no bespoke map erase needed.)
    if (liveEid != coop::element::kInvalidId) {
        if (auto taken = PropMirrors().Take(liveEid))
            coop::element::ElementDeleter::Get().Enqueue(std::move(taken));
        coop::element::ElementDeleter::Get().Flush();
    }

    const bool pass = (reaped >= 1) && initCleared && eidFreed && livePreserved;
    UE_LOGI("propreap_test: forced-dead reap check %s -- reaped=%zu initCleared=%d eidFreed=%d livePreserved=%d (deadEid=%u, liveEid=%u, isHost=%d)",
            pass ? "PASS" : "FAIL", reaped, initCleared ? 1 : 0, eidFreed ? 1 : 0,
            livePreserved ? 1 : 0, deadEid, liveEid, isHost ? 1 : 0);
    return pass;
}

}  // namespace coop::prop_element_tracker
