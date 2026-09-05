// coop/props/prop_key_index.cpp -- the key-to-live-actor index of coop::prop_element_tracker
// (see coop/props/prop_element_tracker.h). One TU owns the whole index: the state, the private
// insert and evict helpers the tracker's mark, unmark and reap call (shared through
// prop_element_tracker_detail.h), and the public lookups and collectors built on it. The
// tracker never touches the maps directly.

#include "coop/props/prop_element_tracker.h"

#include "prop_element_tracker_detail.h"  // co-located private header (src tree, not include/)

#include "ue_wrap/engine/engine.h"  // GetActorLocation
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::prop_element_tracker {
namespace {

namespace R = ue_wrap::reflection;

// The key-to-live-actor index, maintained beside the actor-to-element map: every keyed prop
// that commits a Prop element is indexed key to {actor, internal index}, and evicted on
// unmark and on the dead-element reap. It replaces the by-key object-array walk in the
// connect-time spawn dedupe: one full walk with a per-candidate string allocation and a
// key-getter dispatch, per re-snapshotted prop, ballooned the client to gigabytes and stalled
// the session; with the index each dedupe is one hash lookup and one IsLiveByIndex.
// Bidirectional: the forward map for the lookup, the reverse for actor-keyed removal (unmark
// and reap have the actor, not the key). The forward entry caches the object-array index so
// the lookup validates liveness without dereferencing a possibly freed pointer. Keys are not
// unique across a level reload (a purged prop's key can reappear on a fresh actor) and an
// actor address can be recycled, so insert overwrites (the newest live actor wins); a stale
// forward entry surviving an address recycle is harmless, since the by-index check rejects it
// and the lookup evicts it lazily, and removal goes by the actor's current key so it never
// clobbers a newer prop at the same address. The mutex is a leaf: acquired alone, released
// before any engine or registry call, so it cannot invert against a sibling mutex.
struct KeyActorEntry {
    void*   actor       = nullptr;
    int32_t internalIdx = -1;
};
std::mutex g_keyIndexMutex;
std::unordered_map<std::wstring, KeyActorEntry> g_keyToActor;
std::unordered_map<void*, std::wstring> g_actorToKey;

}  // namespace

// Insert or refresh the index for `actor`, both directions. No-op for empty or None keys
// (non-syncable props are never looked up by key). A rekey drops the old forward entry first,
// so it cannot linger pointing at the live actor under a dead key. The caller holds no other
// mutex.
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

// Remove the entries for `actor`, both directions. The forward entry goes only if it still
// points at this actor, so a newer prop that recycled the address is not disturbed. The
// caller holds no other mutex.
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

// The public lookups.

void IndexActorKey(void* actor, const std::wstring& key) {
    if (!actor || key.empty() || key == L"None") return;
    IndexKeyForActor_(actor, key, R::InternalIndexOf(actor));
}

size_t KeyIndexSize() {
    std::lock_guard<std::mutex> lk(g_keyIndexMutex);
    return g_keyToActor.size();
}

void CollectKeyIndexEntries(std::vector<KeyIndexEntry>& out) {
    // The sweep's keyed universe. A leaf-mutex copy; liveness and ownership validation is the
    // caller's (by-index liveness, current-key re-validation before any doom). No engine calls
    // under the lock.
    std::lock_guard<std::mutex> lk(g_keyIndexMutex);
    out.reserve(out.size() + g_keyToActor.size());
    for (const auto& kv : g_keyToActor)
        out.push_back(KeyIndexEntry{kv.second.actor, kv.second.internalIdx, kv.first});
}

void EvictKeyIndexEntryIfStale(void* actor, const std::wstring& key) {
    // Drop a sweep-detected stale pairing (a recycled slot or an un-reindexed rekey); erase only
    // while the maps still hold exactly this pair, so a concurrent re-index of either side is
    // never clobbered.
    if (!actor || key.empty()) return;
    std::lock_guard<std::mutex> lk(g_keyIndexMutex);
    auto kit = g_keyToActor.find(key);
    if (kit != g_keyToActor.end() && kit->second.actor == actor) g_keyToActor.erase(kit);
    auto ait = g_actorToKey.find(actor);
    if (ait != g_actorToKey.end() && ait->second == key) g_actorToKey.erase(ait);
}

size_t DrainDeadKeyIndexEntries() {
    // The collector for element-less keyed entries: a client's save-loaded keyed props live only
    // in this index (no registry row), so an actor dying without a destroy call (a mass purge, an
    // engine teardown) leaves an entry the element reaper cannot see. Its owners: the post-purge
    // world-change re-seed edge and the stale-index self-heal, both cold, plus the join sweep's
    // per-entry evict and the lookup's lazy evict for the steady trickle. Snapshot under the leaf
    // mutex, validate liveness outside it, erase under it with the same still-this-pair gate.
    std::vector<KeyIndexEntry> snap;
    CollectKeyIndexEntries(snap);
    size_t drained = 0;
    for (const auto& ke : snap) {
        if (!ke.actor) continue;
        if (R::IsLiveByIndex(ke.actor, ke.internalIdx)) continue;
        std::lock_guard<std::mutex> lk(g_keyIndexMutex);
        auto kit = g_keyToActor.find(ke.key);
        if (kit != g_keyToActor.end() && kit->second.actor == ke.actor &&
            kit->second.internalIdx == ke.internalIdx) {
            g_keyToActor.erase(kit);
            ++drained;
        }
        auto ait = g_actorToKey.find(ke.actor);
        if (ait != g_actorToKey.end() && ait->second == ke.key) g_actorToKey.erase(ait);
    }
    return drained;
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
    // Validate liveness without dereferencing the cached pointer (it may have been freed since
    // indexing): the by-index check reads only the object-array slot. A stale entry reports not
    // live; evict it lazily so a recycle never looked up again cannot accumulate, then return
    // null and the caller falls back to the cold scan.
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
    // The cold fallback: a prop that exists locally but is not indexed yet; the object-array scan
    // runs only on an index miss. A successful fallback means the index was stale (it points at
    // dead actors after a world-change purge the slow re-seed has not caught up with), and the
    // caller uses the flag to trigger a throttled re-seed so the rest of a snapshot burst
    // resolves in constant time. A scan miss leaves the flag false: nothing to re-seed, the
    // caller spawns.
    void* scanned = ue_wrap::prop::FindByKeyString(key);
    if (scanned && outFellBackToScan) *outFellBackToScan = true;
    return scanned;
}

bool ReconcileIndexThrottled() {
    // The self-heal for a stale key index detected mid-dedupe (a world change purged the indexed
    // actors and the steady-state re-seed, gated on the slow reaper, has not caught up): drain
    // the dead Prop elements, then re-seed, so the index reflects the current world and the
    // in-flight burst dedupes in constant time. Throttled so only the first stale fallback in a
    // burst pays the full re-seed walk. Game thread only.
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
    // Also the element-less keyed entries, which the element reap above cannot see.
    drained += DrainDeadKeyIndexEntries();
    const size_t added = ReSeedKnownKeyedProps();
    UE_LOGI("prop_element_tracker: stale-index self-heal -- drained %zu dead, re-seeded %zu new keyed prop(s) into the key index (snapshot de-dupe now O(1))",
            drained, added);
    return true;
}

void CollectTrackedKeyedPropKeys(std::unordered_set<std::wstring>& out) {
    // The index holds exactly the live keyed props that minted a Prop element with a non-empty
    // wire key, the keyed set the save persists; keyless chipPiles never enter it, which is the
    // key diff's scope. A leaf-mutex copy; no engine calls under the lock.
    std::lock_guard<std::mutex> lk(g_keyIndexMutex);
    out.reserve(out.size() + g_keyToActor.size());
    for (const auto& kv : g_keyToActor) out.insert(kv.first);
}

void CollectTrackedKeyedPropTransforms(
    std::unordered_map<coop::element::ElementId, ue_wrap::FVector>& out) {
    // The host's save-time keyed-prop positions by host eid (see the header). Copy the actor set
    // under the leaf mutex, then read the eid and position outside it. No self-seed needed: keyed
    // props are index-tracked by the connect seed; an unindexed one is skipped, since only a
    // host-moved prop needs this correction and a moved prop is tracked.
    struct KeyedActor { void* actor; int32_t idx; };
    std::vector<KeyedActor> actors;
    {
        std::lock_guard<std::mutex> lk(g_keyIndexMutex);
        actors.reserve(g_keyToActor.size());
        for (const auto& kv : g_keyToActor)
            actors.push_back({kv.second.actor, kv.second.internalIdx});
    }
    for (const KeyedActor& ka : actors) {
        // By index, not the raw liveness check: the index may hold a recycled slot, which the
        // by-index check rejects.
        if (!ka.actor || !R::IsLiveByIndex(ka.actor, ka.idx)) continue;
        const coop::element::ElementId eid = GetPropElementIdForActor(ka.actor);
        if (eid == coop::element::kInvalidId || eid == 0u) continue;
        out[eid] = ue_wrap::engine::GetActorLocation(ka.actor);
    }
}

}  // namespace coop::prop_element_tracker
