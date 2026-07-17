// coop/props/prop_key_index.cpp -- the key -> live-actor index family of
// coop::prop_element_tracker (see coop/props/prop_element_tracker.h).
//
// Extracted from prop_element_tracker.cpp 2026-07-10 when the tracker passed
// the 800-LOC soft cap (the census walk left first; this is the second slice).
// Behavior preserved byte-for-byte: same mutex scopes, same lazy-evict /
// overwrite / ownership-gate semantics, same throttle window.
//
// One TU owns the whole index: the state (g_keyToActor / g_actorToKey), the
// private insert/evict helpers the tracker's Mark/Unmark/Reap call (shared via
// prop_element_tracker_detail.h), and the public lookups + collectors built on
// it. The tracker never touches the maps directly.

#include "coop/props/prop_element_tracker.h"

#include "prop_element_tracker_detail.h"  // co-located private header (src tree, not include/)

#include "ue_wrap/engine/engine.h"  // GetActorLocation (F1 keyed save-time map)
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

}  // namespace

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

void CollectTrackedKeyedPropTransforms(
    std::unordered_map<coop::element::ElementId, ue_wrap::FVector>& out) {
    // F1 (2026-07-09): host save-time KEYED-prop positions by host eid (see the header). The key
    // index g_keyToActor holds exactly the live keyed Aprops the save persists (keyless chipPiles
    // never enter it). Copy the actor set under the leaf mutex, then read eid + pos OUTSIDE the lock
    // (no engine calls under the mutex -- same discipline as CollectTrackedKeyedPropKeys). No self-seed
    // needed: keyed props are already index-tracked by the connect seed (that is what the R2 key-diff +
    // the snapshot walk both rely on); an unindexed keyed prop is simply skipped (the snapshot's own
    // pos still applies for it -- only a HOST-MOVED prop needs this correction, and a moved prop is
    // tracked).
    struct KeyedActor { void* actor; int32_t idx; };
    std::vector<KeyedActor> actors;
    {
        std::lock_guard<std::mutex> lk(g_keyIndexMutex);
        actors.reserve(g_keyToActor.size());
        for (const auto& kv : g_keyToActor)
            actors.push_back({kv.second.actor, kv.second.internalIdx});
    }
    for (const KeyedActor& ka : actors) {
        // IsLiveByIndex (not raw IsLive): the index may hold a recycled slot; the by-index check rejects it.
        if (!ka.actor || !R::IsLiveByIndex(ka.actor, ka.idx)) continue;
        const coop::element::ElementId eid = GetPropElementIdForActor(ka.actor);
        if (eid == coop::element::kInvalidId || eid == 0u) continue;
        out[eid] = ue_wrap::engine::GetActorLocation(ka.actor);
    }
}

}  // namespace coop::prop_element_tracker
