// coop/props/prop_tracked_keys.cpp -- the tracked keyed props of coop::prop_element_tracker, by key
// and by actor (see coop/props/prop_element_tracker.h). One TU owns the whole map: the state, the
// private insert and evict helpers the tracker's mark, unmark and reap call (shared through
// prop_element_tracker_detail.h), and the public lookups and collectors built on it. The tracker
// never touches the maps directly.

#include "coop/props/prop_element_tracker.h"

#include "prop_element_tracker_detail.h"  // co-located private header (src tree, not include/)


#include "ue_wrap/engine/engine.h"  // TryGetActorLocation
#include "ue_wrap/core/log.h"
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

// The tracked keyed props, kept beside the actor-to-element map: every keyed prop that commits a
// Prop element, a client's census-tracked prop and a bound wire mirror are filed key to {actor,
// internal index}, and evicted on unmark and on the dead-element reap. The enumerations below read
// it: the join sweep's keyed universe, the save-transfer key diff, the save-time positions. A key on
// the wire is resolved by coop/props/prop_key_index, not here. Bidirectional: the forward map is what
// the enumerations count, one actor per key (insert overwrites, the newest wins), and the reverse
// serves actor-keyed removal (unmark and reap have the actor, not the key). The forward entry caches
// the object-array index so a reader validates liveness without dereferencing a possibly freed
// pointer; removal goes by the actor's current key, so it never clobbers a newer prop at the same
// address. The mutex is a leaf: acquired alone, released before any engine or registry call, so it
// cannot invert against a sibling mutex.
struct KeyActorEntry {
    void*   actor       = nullptr;
    int32_t internalIdx = -1;
};
std::mutex g_trackedKeysMutex;
std::unordered_map<std::wstring, KeyActorEntry> g_keyToActor;
std::unordered_map<void*, std::wstring> g_actorToKey;

}  // namespace

// Insert or refresh the index for `actor`, both directions. No-op for empty or None keys
// (non-syncable props are never looked up by key). A rekey drops the old forward entry first,
// so it cannot linger pointing at the live actor under a dead key. The caller holds no other
// mutex.
void TrackKeyForActor_(void* actor, const std::wstring& key, int32_t internalIdx) {
    if (!actor || key.empty() || key == L"None") return;
    std::lock_guard<std::mutex> lk(g_trackedKeysMutex);
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
void EraseTrackedKeyForActor_(void* actor) {
    if (!actor) return;
    std::lock_guard<std::mutex> lk(g_trackedKeysMutex);
    auto ait = g_actorToKey.find(actor);
    if (ait == g_actorToKey.end()) return;
    auto kit = g_keyToActor.find(ait->second);
    if (kit != g_keyToActor.end() && kit->second.actor == actor) {
        g_keyToActor.erase(kit);
    }
    g_actorToKey.erase(ait);
}

// The public lookups.

void TrackActorKey(void* actor, const std::wstring& key) {
    if (!actor || key.empty() || key == L"None") return;
    TrackKeyForActor_(actor, key, R::InternalIndexOf(actor));
}

void CollectTrackedKeyEntries(std::vector<TrackedKeyEntry>& out) {
    // The sweep's keyed universe. A leaf-mutex copy; liveness and ownership validation is the
    // caller's (by-index liveness, current-key re-validation before any doom). No engine calls
    // under the lock.
    std::lock_guard<std::mutex> lk(g_trackedKeysMutex);
    out.reserve(out.size() + g_keyToActor.size());
    for (const auto& kv : g_keyToActor)
        out.push_back(TrackedKeyEntry{kv.second.actor, kv.second.internalIdx, kv.first});
}

void EvictTrackedKeyIfStale(void* actor, const std::wstring& key) {
    // Drop a sweep-detected stale pairing (a recycled slot or an un-reindexed rekey); erase only
    // while the maps still hold exactly this pair, so a concurrent re-index of either side is
    // never clobbered.
    if (!actor || key.empty()) return;
    std::lock_guard<std::mutex> lk(g_trackedKeysMutex);
    auto kit = g_keyToActor.find(key);
    if (kit != g_keyToActor.end() && kit->second.actor == actor) g_keyToActor.erase(kit);
    auto ait = g_actorToKey.find(actor);
    if (ait != g_actorToKey.end() && ait->second == key) g_actorToKey.erase(ait);
}

size_t DrainDeadTrackedKeys() {
    // The collector for element-less keyed entries: a client's save-loaded keyed props live only
    // in this map (no registry row), so an actor dying without a destroy call (a mass purge, an
    // engine teardown) leaves an entry the element reaper cannot see. Its owners: the post-purge
    // world-change re-seed edge, cold, and the join sweep's per-entry evict. Snapshot under the leaf
    // mutex, validate liveness outside it, erase under it with the same still-this-pair gate.
    std::vector<TrackedKeyEntry> snap;
    CollectTrackedKeyEntries(snap);
    size_t drained = 0;
    for (const auto& ke : snap) {
        if (!ke.actor) continue;
        if (R::IsLiveByIndex(ke.actor, ke.internalIdx)) continue;
        std::lock_guard<std::mutex> lk(g_trackedKeysMutex);
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

void CollectTrackedKeyedPropKeys(std::unordered_set<std::wstring>& out) {
    // The tracked keys hold the live keyed props that minted a Prop element with a non-empty wire
    // key, the keyed set the save persists; keyless chipPiles never enter it, which is the
    // key diff's scope. A leaf-mutex copy; no engine calls under the lock.
    std::lock_guard<std::mutex> lk(g_trackedKeysMutex);
    out.reserve(out.size() + g_keyToActor.size());
    for (const auto& kv : g_keyToActor) out.insert(kv.first);
}

void CollectTrackedKeyedPropTransforms(
    std::unordered_map<coop::element::ElementId, ue_wrap::FVector>& out) {
    // The host's save-time keyed-prop positions by host eid (see the header). Copy the actor set
    // under the leaf mutex, then read the eid and position outside it. No self-seed needed: keyed
    // props are tracked by the connect seed; an untracked one is skipped, since only a
    // host-moved prop needs this correction and a moved prop is tracked.
    struct KeyedActor { void* actor; int32_t idx; };
    std::vector<KeyedActor> actors;
    {
        std::lock_guard<std::mutex> lk(g_trackedKeysMutex);
        actors.reserve(g_keyToActor.size());
        for (const auto& kv : g_keyToActor)
            actors.push_back({kv.second.actor, kv.second.internalIdx});
    }
    for (const KeyedActor& ka : actors) {
        // By index, not the raw liveness check: the map may hold a recycled slot, which the by-index
        // check rejects.
        if (!ka.actor || !R::IsLiveByIndex(ka.actor, ka.idx)) continue;
        const coop::element::ElementId eid = GetPropElementIdForActor(ka.actor);
        if (eid == coop::element::kInvalidId || eid == 0u) continue;
        ue_wrap::FVector at{};
        if (!ue_wrap::engine::TryGetActorLocation(ka.actor, at)) continue;   // no read, no save-time position
        out[eid] = at;
    }
}

}  // namespace coop::prop_element_tracker
