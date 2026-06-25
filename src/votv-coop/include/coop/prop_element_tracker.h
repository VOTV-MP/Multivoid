// coop/prop_element_tracker.h -- per-actor lifecycle bookkeeping for keyed
// interactables (Aprop_C + chipPile/clump/trashBitsPile families).
//
// Three independent maintained sets, all keyed by AActor*:
//
//   1. ProcessedInit set
//      Dedupes the "Init POST observer fires twice via a BP Super call"
//      pattern. Entries added on Init POST broadcast, removed on
//      K2_DestroyActor PRE. Cap 16384.
//
//   2. KnownKeyedProps set
//      The maintained live-actor set. Seeded ONCE at Install via a single
//      GUObjectArray walk; thereafter maintained by Init POST (insert) +
//      K2_DestroyActor PRE (evict). Replaces the per-reconnect GUObjectArray
//      walk that the H2-redux work retired on 2026-05-28.
//
//   3. PropElement shadow (the element::Prop owner -- PR-FOUNDATION-3 Inc3)
//      Each entry in KnownKeyedProps gets a parallel coop::element::Prop
//      Element. The Element is OWNED by the shared singleton
//      coop::element::MirrorManager<Prop>::Instance() (the SAME manager
//      remote_prop uses for wire mirrors) -- the bespoke g_propElementsById
//      owner map is retired (RULE 2). This module keeps just ONE bespoke map:
//      g_actorToPropElementId (actor* -> local eid), the reverse lookup the
//      destroy gate + the Init-POST broadcast elementId stamp need. Lifetime
//      mirrors npc_sync: MarkPropElement AllocAndInstall's the Element into
//      the manager (m_mirror=false -> dtor FreeId) + records the reverse
//      entry; UnmarkKnownKeyedProp erases the reverse entry then Take's the
//      Element out of the manager + lets the dtor run outside the lock
//      (ABBA-safe; FreeId acquires element::Registry::m_mutex).
//
// Extracted from prop_lifecycle.cpp 2026-05-29 (M-1 follow-up to prop_synth_key);
// owner-map migrated to MirrorManager<Prop> 2026-05-30 (PR-FOUNDATION-3 Inc3).
// Behavior preserved: same idempotency / cap / overflow-log semantics, same
// in-lock role-read for the host/peer-range allocator decision in
// MarkPropElement, same leaf-mutex lock ordering.
//
// Session pointer:
//   prop_element_tracker holds its OWN cached coop::net::Session* (separate
//   from prop_lifecycle's). prop_lifecycle::SetSession + Install +
//   InstallInventory call this module's SetSession too. This keeps the
//   in-lock role read (audit fix 2026-05-29 prior session: capture role
//   inside the same locked block as the idempotency check; reading it after
//   the lock release would race SetSession / role-change).

#pragma once

#include "coop/element/element.h"
#include "ue_wrap/types.h"  // ue_wrap::FVector (v86 Path 1c pile save-time map)

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::net { class Session; }

namespace coop::prop_element_tracker {

// Cache the session pointer. Mirror of prop_lifecycle::SetSession --
// prop_lifecycle's Install / SetSession / InstallInventory call this too
// so the in-lock role read in MarkPropElement / SeedKnownKeyedProps has
// a live pointer to query.
void SetSession(coop::net::Session* session);

// ---- ProcessedInit dedupe ------------------------------------------------
void MarkProcessedInit(void* actor);
bool HasProcessedInit(void* actor);
void UnmarkProcessedInit(void* actor);

// OnDisconnect helper: clear the set, return the count we dropped + reset
// the overflow-logged latch. Called by prop_lifecycle::OnDisconnect.
size_t ClearProcessedInit();

// ---- KnownKeyedProps maintained set --------------------------------------
void MarkKnownKeyedProp(void* actor);

// Drains BOTH the known set AND the Prop Element shadow (one operation
// because they share the actor-keyed lifecycle). Drain-then-destruct
// pattern: extract shadow under lock, release lock, then let the
// unique_ptr destructor run. ABBA-safe vs element::Registry::m_mutex.
void UnmarkKnownKeyedProp(void* actor);

// ---- Prop Element shadow -------------------------------------------------
// Create a Prop Element for `actor`. Idempotent: no-op if already tracked.
// Reads its OWN cached session pointer inside the lock to determine host
// vs peer allocation range. Names + class name optional (empty wstring
// skips the field set).
void MarkPropElement(void* actor, const std::wstring& key, const std::wstring& cls);

// Look up the host-allocated ElementId for `actor`. Returns kInvalidId if
// not tracked.
coop::element::ElementId GetPropElementIdForActor(void* actor);

// ---- Phase 1 step 2b: bound-mirror guard (save_identity_bind) -------------
// Mark `actor` as bound to a host-range MIRROR by the eid-range bind, so MarkPropElement / SeedWalk_ will NOT
// re-mint a peer-range LOCAL element on it at the next post-load re-seed (which would double-element + create a
// sweep-doomable local twin). Stored actor->internalIdx so a recycled address self-heals (IsLiveByIndex).
// Game-thread (the bind runs on the BeginDeferred thunk). Cleared by ClearBoundMirrorNatives + UnmarkKnownKeyedProp.
void MarkBoundMirrorNative(void* actor);

// Drop ALL bound-mirror guard flags (save_identity_bind arm / disconnect -- the set is join-scoped).
void ClearBoundMirrorNatives();

// True iff `actor` is currently bound as a host-range mirror (live; self-heals a recycled entry). The bind
// uses it to ignore a double-fire of the BeginDeferred thunk for an already-bound native (no double-bind).
bool IsBoundMirrorNative(void* actor);

// Re-point a LOCAL Prop Element (m_mirror=false, owned by THIS peer) from its
// current actor onto `newActor`, keeping the SAME eid. Used by the host-authoritative
// trash channel (trash_channel / remote_prop::OnConvert): when this peer's OWN pile
// (a local tracker Element) re-skins pile-A -> clump -> pile-B, the eid `E` must
// follow the new UObject so the held-pose stream + a later grab/destroy resolve
// it. Updates the Element's cached actor/liveness-index AND the reverse map
// (g_actorToPropElementId): drops the old actor's entry (if it still points at
// `eid`) and binds `newActor -> eid`. No-op for an unknown eid / a mirror eid
// (mirrors are rebound by remote_prop::RegisterPropMirror with rebindInPlace).
// Game-thread only (the morph edges all run on the game thread). Keyless props
// only -- it does NOT touch the key index (chipPile/clump carry Key=None).
void RebindLocalElementActor(coop::element::ElementId eid, void* newActor);

// ---- Key -> live-actor index --------------------------------------------
// O(1) resolution of a wire Key string to the live local Aprop_C* (and the
// chipPile/clump/trashBits keyed-interactable families). THE replacement for the
// per-call ue_wrap::prop::FindByKeyString GUObjectArray walk that made the
// connect-time re-snapshot de-dupe O(N_props x N_objects) -- ~2316 incoming
// PropSpawns x a ~150k-object scan-with-wstring-alloc each ballooned the client
// to multi-GB on connect (the [[project-bug-prop-resnapshot-leak]] ship-blocker).
//
// The index is maintained automatically by MarkPropElement (insert) +
// UnmarkKnownKeyedProp / ReapDeadLocalPropElements (evict), so every keyed prop
// that has a Prop Element is resolvable here.

// Resolve `key` to a LIVE local actor via the maintained index ONLY (no scan).
// Returns nullptr on a miss. Validates liveness via IsLiveByIndex (no deref of a
// possibly-freed pointer) and lazily evicts a stale entry it finds.
void* FindLiveActorByKey(const std::wstring& key);

// Resolve `key` to a LIVE local actor: O(1) index first, then a cold
// ue_wrap::prop::FindByKeyString GUObjectArray scan on an index miss so behavior
// is identical to the pre-index path for not-yet-indexed props. This is the
// drop-in replacement the wire receivers (remote_prop OnSpawn / drive / destroy)
// call instead of FindByKeyString directly. Game-thread + worker safe.
//
// outFellBackToScan (optional): set true iff the index MISSED but the cold scan
// FOUND a live actor -- i.e. the index was STALE for an existing prop (a
// world-change purged the indexed actors faster than the steady-state re-seed
// rebuilt the index). The snapshot de-dupe site passes this and calls
// ReconcileIndexThrottled() on a true result so the rest of an in-flight de-dupe
// burst resolves O(1) instead of each falling back to the O(N) scan (the
// [[project-bug-prop-resnapshot-leak]] balloon). Left false on an index hit or a
// genuine miss (prop absent -> nothing to re-seed).
void* ResolveLiveActorByKey(const std::wstring& key, bool* outFellBackToScan = nullptr);

// Self-heal for a stale key index detected mid-de-dupe: throttled (>=200 ms)
// drain-dead + re-seed so the index reflects the CURRENT loaded world. Returns
// true if it actually reconciled (false if throttled out). Game-thread only.
// Called by the snapshot receiver when ResolveLiveActorByKey reports a stale
// fallback; idempotent vs the net_pump world-change re-seed episode path.
bool ReconcileIndexThrottled();

// Refresh the index for `actor` to `key` (e.g. after a fuzzy-match rekey that
// changed the actor's Key without re-running MarkPropElement). Idempotent;
// no-op for empty/None keys. Keeps the index hot so a rekeyed held prop's
// per-grab resolution stays O(1) instead of perpetually falling back to scan.
void IndexActorKey(void* actor, const std::wstring& key);

// ---- R2: blob-vs-live divergence baseline (2026-06-17) -------------------
// Copy the wire-keys of all currently-tracked keyed props (the host's live
// keyed-prop set) into `out`. save_transfer snapshots this at blob-capture
// (OnRequest) and again at the connect edge, diffing the two to send EXPLICIT
// per-key PropDestroy for props the blob HAD that the host has since removed
// (MTA Packet_EntityRemove) -- so the joining client never INFERS a delete via
// the divergence sweep. Keyless chipPiles are NOT in the key index (their
// cross-peer identity is the eid, not a key), so this is naturally
// keyed-Aprop_C only. O(tracked) copy under the key-index leaf mutex; no
// reflection, no GUObjectArray walk. Game-thread or worker safe.
void CollectTrackedKeyedPropKeys(std::unordered_set<std::wstring>& out);

// v86 Path 1c: collect the SAVE-TIME position of every live tracked chipPile,
// keyed by its host ElementId. Called once at the scratch-save instant
// (save_transfer::OnRequest) so the host can later stamp each pile's snapshot
// with the position the client loaded it at (the cross-peer-stable key the
// join-window two-channel DUP fix matches on). KEYLESS chipPiles only (the
// thing that duped); an unseeded pile (no eid yet) is skipped -> no key, the
// receiver falls back to the current pose for it. One GUObjectArray walk on the
// game thread (same cold connect-edge cost as CollectTrackedKeyedPropKeys).
void CollectTrackedPileTransforms(
    std::unordered_map<coop::element::ElementId, ue_wrap::FVector>& out);

// scope A (kerfur off->active dup retire, 2026-06-24): collect the SAVE-TIME position of every live
// OFF-FORM kerfur (prop_kerfurOmega_C + skins), keyed by its host ElementId. Same blob-instant capture
// + self-seed (mint an unseeded eid inline, idempotent) as CollectTrackedPileTransforms, but gated to
// the kerfur prop lineage. The host carries this position onto the KerfurConvert when it turns the
// kerfur ON in the join window, so the joining client can RETIRE its stale local off-prop (whose host
// no longer expresses it as off) matched at the exact save-time key. One GUObjectArray walk, game
// thread, cold connect-edge cost.
void CollectTrackedKerfurTransforms(
    std::unordered_map<coop::element::ElementId, ue_wrap::FVector>& out);

// ---- One-shot seed -------------------------------------------------------
// GUObjectArray walk that populates KnownKeyedProps + creates Prop Element
// shadows for every live keyed-interactable. Internal latch; safe to
// call multiple times. Two-phase: phase 1 walks without lock, phase 2
// takes the mutex for bulk insert (avoids holding the mutex for the full
// ~150k object walk).
void SeedKnownKeyedProps();

// Re-run the seed walk WITHOUT the one-shot latch (snapshot-completeness fix,
// 2026-05-30). The boot SeedKnownKeyedProps runs ONCE, before VOTV's boot-time
// level travel (`open untitled_1` into the story map); after the travel its
// captured actors are all dead and the new level's PLACED props don't fire a
// catchable Init POST (which is WHY the seed exists), so the host ends up
// tracking only the ~70 runtime-spawned props instead of ~2000 -> the
// late-joiner snapshot ships ~70 props. This re-walks GUObjectArray and adds any
// live keyed-interactable the world has gained that we are not yet tracking.
// Fully idempotent (set-insert + idempotent MarkPropElement) so it is safe to
// call repeatedly; pairs with ReapDeadLocalPropElements (which drains the old
// level's dead shadows). Returns the number of NEW props added to tracking.
// Game-thread only; expensive (full ~150k GUObjectArray walk) -- call on a
// world/level-change edge, never per-tick. This is also the re-seed a future
// cave/level-travel feature needs.
//
// R1 (2026-06-17, MTA CEntityAddPacket streaming): optionally YIELDS the actors
// this walk newly adopted. The steady-world re-seed (net_pump) passes a vector
// and broadcasts ONE incremental PropSpawn per new actor (prop_snapshot::
// ExpressIncrementalSpawn) instead of re-firing the whole bracketed snapshot --
// a bracket re-arms the client divergence sweep, an incremental add does not.
// nullptr (boot seed / throttled reconcile) keeps the prior count-only behavior.
size_t ReSeedKnownKeyedProps(std::vector<void*>* outNewActors = nullptr);

// ---- World-coherence stamp (Fork A, 2026-06-10) ---------------------------
// Every seed walk stamps the live gameplay UWorld it expressed; the snapshot
// trigger gate refuses to open a bracket (a DESTRUCTIVE contract: the client
// sweeps unclaimed locals at SnapshotComplete) unless the registry's stamped
// world is still the live one. GT-read; O(1) (IsLiveByIndex on the captured
// pointer -- a world swap's GC purge kills the stamp with zero latency).
bool HasSeededOnce();                   // the boot-seed latch, promoted
bool IsRegistrySeededForCurrentWorld(); // stamp non-null && IsLiveByIndex(stamp)
uint64_t SeedGeneration();              // bumps at the tail of EVERY seed walk

// Purge-episode flag (gate hardening, 2026-06-10): true between the reaper's
// mass-purge detection and the episode-end re-seed -- the registry is
// draining a dead world's elements and must not be snapshot-expressed. The
// world-stamp alone is insufficient: VOTV's boot/save-load can leave the
// stamped UWorld alive while the registry is majority-dead (smoke-proven).
// net_pump owns the detection edges and calls the setter; the snapshot gate
// reads it.
void SetInPurgeEpisode(bool active);
bool InPurgeEpisode();

// ---- Dead-Element reaper (PR-FOUNDATION, 2026-05-30) ----------------------
// Reconcile the LOCAL Prop Element shadows against the live world: any local
// (non-mirror) Prop Element whose backing actor the engine has purged
// (reflection::IsLiveByIndex == false on the cached index) is evicted exactly
// as its K2_DestroyActor PRE would have -- drained out of the canonical
// MirrorManager<Prop> by eid and routed through ElementDeleter -- minus the
// wire PropDestroy (a mass purge is engine teardown, not a gameplay destroy to
// replicate). Needed because a mass GC purge (cave/level transition, save-load)
// flags ~2000 props PendingKill AT ONCE without firing per-actor
// K2_DestroyActor, so the sole eviction path is bypassed and the dead shadows
// leak: unbounded across transitions, and after ~7 the 16384 KnownKeyedProps /
// ProcessedInit caps exhaust and NEW props stop being tracked -- silently
// breaking the late-joiner snapshot and live prop-spawn replication.
//
// Reaps by EID with an IsMirror() gate (robust against the engine recycling a
// purged actor's address for a new keyed prop -- an actor-keyed evict could
// then kill the live new Element). Caps at `maxEvictions` per call so a
// post-purge backlog drains across a handful of throttled scans instead of one
// frame. No-op (returns 0) when nothing is dead. Game-thread only (mirrors the
// K2_DestroyActor PRE eviction site + the ElementDeleter::Flush contract).
// Returns the number of dead local Prop Elements evicted.
//
// PART 1 (2026-06-18, host-authoritative death-watch): optionally YIELDS the eid of each
// evicted element. The net_pump reaper, in STEADY state (not a mass-purge transition),
// broadcasts an explicit PropDestroy(eid) per yielded eid on the HOST -- so a prop/pile the
// host destroyed via an un-hookable BP-internal EX_CallMath path (garbage-truck collect,
// ambient cull, removeWOrespawn/LifeSpan despawn) -- which NEVER fires K2_DestroyActor and
// so was only ever cleaned by the retired 4s full re-snapshot -- now propagates as an
// explicit per-entity remove (MTA Packet_EntityRemove), by identity, not a proximity guess.
// nullptr (the boot/self-test callers) keeps the prior silent-eviction behavior.
size_t ReapDeadLocalPropElements(size_t maxEvictions,
                                 std::vector<coop::element::ElementId>* outReapedEids = nullptr);

// Self-test (env VOTVCOOP_RUN_PROPREAP_TEST): construct a synthetic DEAD local
// Prop Element (sentinel actor + internalIdx -1 so IsLiveByIndex rejects it
// without any deref), verify ReapDeadLocalPropElements evicts it + clears all
// three actor-keyed maps + frees the eid after a Flush, and verify it leaves a
// LIVE local Prop Element untouched. Logs PASS/FAIL. Game-thread only. Proves
// the reap mechanism without the (hard-to-reproduce) natural mass-purge.
bool DebugCheckPropElementReap();

}  // namespace coop::prop_element_tracker
