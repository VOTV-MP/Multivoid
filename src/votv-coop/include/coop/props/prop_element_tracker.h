// coop/props/prop_element_tracker.h -- per-actor lifecycle bookkeeping for keyed interactables (the
// Aprop_C, chipPile, clump and trashBitsPile families): three maintained sets keyed by actor. The
// ProcessedInit set dedupes an Init POST that fires twice through a BP Super call (cap 16384). The
// KnownKeyedProps set is the live-actor set, seeded once by a GUObjectArray walk and maintained by
// the Init POST (insert) and the K2_DestroyActor PRE (evict). The Prop element shadow gives each
// known actor a coop::element::Prop owned by the shared MirrorManager<Prop>, the same manager the
// wire mirrors live in; the actor-to-eid reverse is the unified Registry reverse, and
// GetPropElementIdForActor answers for locals only (a mirror reads kInvalidId). MarkPropElement
// installs the element into the manager (its dtor frees the id and clears the reverse);
// UnmarkKnownKeyedProp takes it out under the lock and lets the dtor run outside it (the Registry
// mutex is acquired by FreeId). The module caches its own Session pointer so the role is read
// inside the same locked block as the idempotency check.

#pragma once

#include "coop/element/element.h"
#include "ue_wrap/core/types.h"  // ue_wrap::FVector

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::net { class Session; }

namespace coop::prop_element_tracker {

// Caches the session pointer; prop_lifecycle's Install, SetSession and InstallInventory call it, so
// the in-lock role read in MarkPropElement and the seed has a live pointer.
void SetSession(coop::net::Session* session);

// The role off the cached session, for the identity-authority branches; with no session both are
// false and neither branch fires.
bool SessionIsHost();
bool SessionIsClient();

// The ProcessedInit dedupe.
void MarkProcessedInit(void* actor);
bool HasProcessedInit(void* actor);
void UnmarkProcessedInit(void* actor);

// Clears the set and resets the overflow-logged latch; returns the count dropped.
size_t ClearProcessedInit();

// The KnownKeyedProps set.
void MarkKnownKeyedProp(void* actor);

// Drains the known set and the Prop element shadow together: the element is extracted under the
// lock and its destructor runs after the release, so the Registry mutex is never taken inside this
// one.
void UnmarkKnownKeyedProp(void* actor);

// The Prop element shadow: a Prop element for `actor`, idempotent, allocated in the host or peer
// range by the role read inside the lock; name and class optional. Returns the key the actor is
// actually enrolled under: the game's own save ships duplicate keys (65 trashBitsPile_C on one
// GUID), and the host re-keys a true duplicate (a different live actor already carries the key)
// before enrolling, so a broadcast builds its payload from the return, never the input. Every
// caller names its enrolment source: an explicit express seam (the Init broadcast, a container
// extract, a held-item express, a pile self-seed) mints and announces on any peer, while the
// passive census walk on a client mints nothing for a keyed prop (keyed identity is
// host-authored: a save-loaded prop adopts the host eid by key, a client-born prop mints at its
// express seam) and only refreshes the key index; a silent local eid nobody was told about was a
// zombie double-row factory, thousands per join.
enum class EnrollSource : uint8_t { kExpressSeam, kPassiveCensus };
std::wstring MarkPropElement(void* actor, const std::wstring& key, const std::wstring& cls,
                             EnrollSource src);

// The local element id for `actor`, or kInvalidId.
coop::element::ElementId GetPropElementIdForActor(void* actor);

// True only for a save-loaded native bound as a host-range mirror by the eid-range bind
// (save_identity_bind) and live (a recycled entry self-heals). Read from Element::IsSaveNative
// through the unified reverse, not a separate set, so it cannot be stale relative to the binding.
// MarkPropElement and the seed skip re-minting a local element on a bound native; the pile and
// kerfur reconciles exclude one from the twin doom set. Set right after RegisterPropMirror; dies
// with the element.
bool IsBoundMirrorNative(void* actor);

// Re-points a local Prop element from its actor onto `newActor`, keeping the eid: when this
// peer's own pile re-skins (pile, clump, pile again) the eid follows the new object so the
// held-pose stream and a later grab or destroy resolve it. The Registry reverse follows through
// SetActor. A no-op for an unknown or mirror eid (a mirror rebinds through RegisterPropMirror
// with rebindInPlace). Keyless props only; the key index is untouched. Game thread.
void RebindLocalElementActor(coop::element::ElementId eid, void* newActor);

// The key-to-live-actor index: O(1) resolution of a wire key to the local actor, replacing the
// per-call GUObjectArray walk that made the connect-time dedupe O(props x objects) and ballooned
// a client to gigabytes on connect. Maintained by MarkPropElement (insert) and
// UnmarkKnownKeyedProp and the reaper (evict).

// `key` to a live local actor through the index only; null on a miss. Liveness by index, and a
// stale entry found on the way is evicted.
void* FindLiveActorByKey(const std::wstring& key);

// `key` to a live local actor: the index first, then a cold GUObjectArray scan on a miss, so a
// not-yet-indexed prop still resolves; the wire receivers call this. Game thread or worker.
// outFellBackToScan is set only when the index missed and the scan found the actor, a stale index
// (a world change purged the indexed actors faster than the re-seed rebuilt it); the snapshot
// dedupe then calls ReconcileIndexThrottled so the rest of the burst resolves O(1).
void* ResolveLiveActorByKey(const std::wstring& key, bool* outFellBackToScan = nullptr);

// The self-heal for a stale index: a drain of the dead entries and a re-seed, throttled to one per
// 200 ms; true when it ran. Game thread.
bool ReconcileIndexThrottled();

// Re-indexes `actor` under `key` (after a fuzzy-match rekey, say), so a rekeyed held prop resolves
// O(1) per grab; idempotent, a no-op for an empty or None key.
void IndexActorKey(void* actor, const std::wstring& key);

// The join sweep's keyed universe: a client's save-loaded keyed props have no element row, the
// index is their tracked membership, so the sweep adjudicates them from these entries (the row
// walk owns the element-bound ones). A snapshot copy under the leaf mutex; validate each by index
// before use.
struct KeyIndexEntry {
    void*        actor       = nullptr;
    int32_t      internalIdx = -1;
    std::wstring key;
};
void CollectKeyIndexEntries(std::vector<KeyIndexEntry>& out);

// How many keyed props the index holds. Any thread.
size_t KeyIndexSize();

// Erases the index entry for `actor` only if it still maps this key to this actor: a
// recycled-address impostor or an un-reindexed rekey falls out of the sweep universe instead of
// dooming a fresh actor, and the live actor re-enters at the next census.
void EvictKeyIndexEntryIfStale(void* actor, const std::wstring& key);

// Drops every index entry whose actor is dead: an element-less keyed entry has no Registry row,
// so the reaper cannot evict it, and a death without K2_DestroyActor would leak the pair for the
// process. Cold paths: the post-purge re-seed edge and the stale-index self-heal. Returns the
// count. Game thread.
size_t DrainDeadKeyIndexEntries();

// The wire keys of every tracked keyed prop, the host's live set. save_transfer snapshots it at
// the blob capture and again at the connect edge and sends an explicit PropDestroy per key the
// blob had that the host has since removed, so the joiner never infers a delete. Keyless
// chipPiles are not in the index (their identity is the eid). A copy under the leaf mutex; no
// reflection.
void CollectTrackedKeyedPropKeys(std::unordered_set<std::wstring>& out);

// The save-time position of every live tracked chipPile by host eid, captured once at the scratch
// save so the host can stamp each pile's snapshot with the position the client loaded it at.
// Keyless piles only; an unseeded pile is skipped and the receiver uses the current pose. One
// GUObjectArray walk on the game thread, at the connect edge.
void CollectTrackedPileTransforms(
    std::unordered_map<coop::element::ElementId, ue_wrap::FVector>& out);

// The save-time position of every live off-form kerfur by host eid, at the same instant and with
// the same inline self-seed of an unseeded eid; the host carries it on a window turn-on's
// KerfurConvert so the joining client retires its stale local off-prop at the exact key. One
// walk, game thread, connect edge.
void CollectTrackedKerfurTransforms(
    std::unordered_map<coop::element::ElementId, ue_wrap::FVector>& out);

// The save-time position of every live keyed prop by host eid: a keyed prop rides the snapshot at
// the host's current position, but the joiner's own loadObjects re-creates it at the save
// position afterwards, so the diverged-position flush re-asserts a host-moved position at
// quiescence. The index copied under the leaf mutex, positions read outside it. Game thread,
// connect edge.
void CollectTrackedKeyedPropTransforms(
    std::unordered_map<coop::element::ElementId, ue_wrap::FVector>& out);

// The one-shot seed: a GUObjectArray walk that populates KnownKeyedProps and creates the Prop
// element shadows for every live keyed interactable; latched, safe to call again. Two phases,
// the walk without the lock and a bulk insert under it.
void SeedKnownKeyedProps();

// The seed walk without the latch. The boot seed runs before VOTV's boot-time level travel, so
// its actors are all dead afterwards and the new level's placed props fire no catchable Init,
// leaving the host tracking a few dozen runtime props instead of about 2,000. This re-walks and
// adds every live keyed interactable not yet tracked; idempotent, paired with the reaper. Returns
// the count added. Expensive: on a world-change edge, never per tick. With `outNewActors` it
// yields the adopted actors, and the steady re-seed broadcasts one incremental PropSpawn each
// (MTA's entity-add streaming) instead of a bracketed snapshot, which would re-arm the client's
// divergence sweep.
size_t ReSeedKnownKeyedProps(std::vector<void*>* outNewActors = nullptr);


// The steady re-seed as a scan-hub consumer: the shared sliced pass collects keyed-interactable
// candidates ({obj, InternalIndex, SlotSerial}), and a budgeted drain (about 1 ms per tick)
// adjudicates them with the seed's own semantics (the set insert under the leaf mutex is the sole
// newness authority; the mint and the late delivery run outside it). Game thread; Install latches,
// Drain returns early on an empty queue.
void InstallReseedScanConsumer();
void DrainReseedQueue();

// The reaper's gameplay-versus-menu verdict, published for the re-seed gate; written by
// registry_reaper only.
void SetReaperInGameplayWorld(bool inGameplay);

// The world-coherence stamp: every seed walk stamps the live gameplay UWorld it expressed, and the
// snapshot trigger refuses to open a bracket (a destructive contract) unless the stamped world is
// still the live one. O(1), by index, so a world swap's purge kills the stamp with no latency.
bool HasSeededOnce();                   // the boot-seed latch
bool IsRegistrySeededForCurrentWorld();  // the stamp is non-null and alive by index
uint64_t SeedGeneration();              // bumps at the tail of every seed walk

// The purge-episode flag: true between the reaper's mass-purge detection and the episode-end
// re-seed, while the registry drains a dead world's elements and must not be expressed. Needed
// beside the stamp, since a save load can leave the stamped UWorld alive while the registry is
// majority-dead. net_pump owns the edges; the snapshot gate reads it.
void SetInPurgeEpisode(bool active);
bool InPurgeEpisode();

// The dead-element reaper: every local Prop element whose actor the engine purged (dead by index)
// is evicted as its K2_DestroyActor PRE would have, drained from the manager by eid through the
// deleter, minus the wire PropDestroy (a mass purge is engine teardown, not a gameplay destroy).
// A level transition flags about 2,000 props PendingKill at once without firing K2_DestroyActor,
// and the leaked shadows exhausted the 16384 caps after a handful of transitions, after which
// new props were silently untracked. Reaps by eid with a mirror gate (a recycled address could
// otherwise kill a live new element); capped per call so a backlog drains over a few scans.
// Game thread. With `outReapedEids` it yields each evicted eid, and the steady-state reaper
// broadcasts a PropDestroy per eid on the host, so a prop the host destroyed through an
// unhookable BP-internal path (a truck collect, an ambient cull, a lifespan despawn) propagates
// by identity.
size_t ReapDeadLocalPropElements(size_t maxEvictions,
                                 std::vector<coop::element::ElementId>* outReapedEids = nullptr);

// The self-test (VOTVCOOP_RUN_PROPREAP_TEST): a synthetic dead local element (a sentinel actor,
// index -1) must be reaped, its maps cleared and its eid freed after a flush, and a live one left
// alone. Logs PASS or FAIL. Game thread.
bool DebugCheckPropElementReap();

}  // namespace coop::prop_element_tracker
