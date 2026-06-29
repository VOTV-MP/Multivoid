// coop/prop_snapshot.cpp -- Phase 5S0 save snapshot bootstrap.
//
// Per-slot drain: snapshot replays to ONE peer slot at a time via
// Session::SendReliableToSlot. Late-joiners get a full snapshot;
// concurrent late-joiners don't race on a shared candidate index.
// Serial drains: if a second peer connects mid-drain, it queues and
// drains after the first completes.
// See coop/prop_snapshot.h for the public interface.

#include "coop/props/prop_snapshot.h"

#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/creatures/kerfur_entity.h"  // IsKerfurActor -- exclude kerfurs from the incremental express (KerfurConvert is their only signal)
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_lifecycle.h"
#include "coop/props/remote_prop.h"
#include "coop/session/save_transfer.h"  // v86 Path 1c: TryGetSaveTimePileXform -- stamp the join snapshot's pile match key
#include "coop/props/snapshot_census.h"  // Phase 0: per-class completeness census appended to SnapshotComplete
#include "coop/dev/eid_lifetime_trace.h"  // Phase 1 S8.2: read-only capture-eid vs wire-eid trace
#include "coop/dev/force_overdestroy_test.h"  // dev-only: inject the over-destroy to PROVE the floor
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::prop_snapshot {
namespace {

namespace R = ue_wrap::reflection;
namespace PT = coop::prop_element_tracker;

// Atomic for symmetry with sibling subsystems (prop_lifecycle, item_activate,
// weather_sync) per [[feedback-install-idempotent-o1-steady-state]]. SetSession
// is called from a non-game-thread context at boot (loader thread), and
// DrainChunk/TriggerForSlot run on the game thread. The current ordering
// (SetSession before Start) makes the bare pointer accidentally safe, but
// the design shouldn't depend on the boot sequence.
std::atomic<coop::net::Session*> g_session_ptr{nullptr};

// Enumeration state for the currently-running drain. Game-thread access
// only; no lock needed.
std::vector<void*> g_snapshotCandidates;
std::vector<coop::element::ElementId> g_snapshotEids;  // parallel; same idx as g_snapshotCandidates
// Parallel to g_snapshotCandidates: each candidate's GUObjectArray InternalIndex
// captured (in the Element) while the actor was live. DrainChunk re-validates
// via R::IsLiveByIndex(actor, idx) so a candidate GC-purged between enumeration
// and its per-tick drain is rejected WITHOUT dereferencing freed actor memory.
std::vector<int32_t> g_snapshotInternalIdxs;
size_t g_snapshotCandidateIdx = 0;
// v34: running count of PropSpawn messages actually sent this drain (<= candidate count
// after wire-suppress / unkeyed skips). Reported in SnapshotComplete for the joiner's
// loading-screen diagnostic. Reset per drain in StartEnumerationFor.
uint32_t g_snapshotSentTotal = 0;
// Which peer slot is being served by the current drain (-1 = no drain
// in progress). Plus the queue of slots waiting their turn (e.g. peer 2
// connecting while peer 1's drain is still in flight).
int g_currentTargetSlot = -1;
std::vector<int> g_pendingSlots;

// Fork A (2026-06-10): slots whose bracket was deferred because the registry
// does not yet express the host's current world (mid world-transition). The
// deferred-slot flush at the top of DrainChunk retries them on every seed-
// generation bump; TriggerForSlot consumes (and possibly re-sets) the flag.
// GT-only like the rest of the drain state.
std::array<bool, coop::players::kMaxPeers> g_deferredSlots{};
uint64_t g_deferredSeenGen = 0;  // SeedGeneration latched at defer; flush fires on the next bump
uint64_t g_drainSeedGen    = 0;  // SeedGeneration captured when the current drain started

bool AnyDeferred_() {
    for (int s = 1; s < coop::players::kMaxPeers; ++s) {
        if (g_deferredSlots[s]) return true;
    }
    return false;
}

void ClearDrainState_();  // defined below (shared by completion/abort/backstop)

// Per-tick drain size -- 100 candidates per frame keeps the per-tick cost
// (~5-15 ms for GetActorLocation/Rotation x100) well under the 16.6 ms
// frame budget. Total ~2000 props @ 100/tick = 20-30 ticks (~330-500 ms
// wall-clock spread across frames, no single-frame stall).
constexpr size_t kSnapshotChunkSize = 100;

// Populate g_snapshotCandidates from prop_lifecycle's maintained
// known-keyed-props set (seeded ONCE at Install via a single
// GUObjectArray walk, then kept current via Init POST insert +
// K2_DestroyActor PRE evict). Replaces the per-reconnect O(150k
// GUObjectArray) walk with an O(~2000 pointer copy under mutex).
// Sets g_currentTargetSlot = peerSlot. Caller (TriggerForSlot or
// post-completion dequeue) ensures no drain is already in progress.
//
// Per-candidate re-validation runs in DrainChunk (IsLive recheck
// covers the window between snapshot-copy and per-tick drain). The
// maintained set is best-effort, not transactional -- a freshly
// destroyed actor may briefly appear here until its K2_DestroyActor
// PRE fires.
void StartEnumerationFor(int peerSlot) {
    g_currentTargetSlot = peerSlot;
    // Fork A: remember which registry generation this drain expresses -- the
    // busy-branch guard in TriggerForSlot dedupes a re-trigger of the SAME
    // slot for the SAME generation (the in-flight bracket already covers it).
    g_drainSeedGen = PT::SeedGeneration();
    g_snapshotCandidates.clear();
    g_snapshotEids.clear();
    g_snapshotInternalIdxs.clear();
    g_snapshotCandidateIdx = 0;
    int skippedDying = 0, skippedDead = 0;
    // Tier 3 Props migration 2026-05-28: enumerate via the unified Element
    // Registry instead of prop_lifecycle's legacy g_knownKeyedProps set.
    // SnapshotActorsByType extracts (actor*, id) pairs under the Registry
    // mutex -- no Element* dereferences happen after the mutex releases,
    // so no use-after-free if another thread frees an Element concurrently.
    //
    // Audit fix 2026-05-28: capture the eid alongside the actor pointer so
    // DrainChunk doesn't re-lock any prop-tracker mutex per candidate
    // (12,500 mutex acquisitions/sec during drain was the prior cost).
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    const size_t trackedCount =
        coop::element::Registry::Get().SnapshotActorsByType(
            coop::element::ElementType::Prop, pairs);
    g_snapshotCandidates.reserve(trackedCount);
    g_snapshotEids.reserve(trackedCount);
    g_snapshotInternalIdxs.reserve(trackedCount);
    for (const auto& pr : pairs) {
        if (!pr.actor) { ++skippedDead; continue; }
        // IsLiveByIndex (NOT IsLive): pr.actor may have been GC-purged since the
        // Element captured it (mass purge doesn't fire K2_DestroyActor, so dying
        // props linger in the Registry). IsLive would deref the freed pointer and
        // AV -- the connect-edge crash root-caused 2026-05-30. IsLiveByIndex reads
        // only the GUObjectArray slot at pr.internalIdx, so a purged actor is
        // rejected cleanly.
        if (!R::IsLiveByIndex(pr.actor, pr.internalIdx)) { ++skippedDying; continue; }
        g_snapshotCandidates.push_back(pr.actor);
        g_snapshotEids.push_back(pr.id);
        g_snapshotInternalIdxs.push_back(pr.internalIdx);
    }
    // Enumerate-time coherence backstop (gate hardening, 2026-06-10): a
    // registry whose DYING elements outnumber its LIVE ones objectively does
    // not express a live world -- it is mid-purge. This closes the <=4s
    // window where a peer connects after a world transition but BEFORE the
    // reaper's next scan raises the purge-episode flag (the trigger gate's
    // signals are all clear in that window). Not a tunable heuristic: in
    // steady gameplay dying ~= 0 (normal destruction is K2-evicted inline),
    // and a sublevel stream-out burst is small against the ~2-3k live world,
    // so majority-dead can only mean a world transition.
    if (skippedDying > static_cast<int>(g_snapshotCandidates.size())) {
        UE_LOGW("snapshot: registry is majority-dead (%d dying vs %zu live) -- mid world-transition; "
                "DEFERRING slot %d instead of bracketing",
                skippedDying, g_snapshotCandidates.size(), peerSlot);
        g_deferredSlots[peerSlot] = true;
        g_deferredSeenGen = PT::SeedGeneration();
        ClearDrainState_();
        return;
    }
    UE_LOGI("snapshot: enumerated %zu live candidates for slot %d from element::Registry (%zu Prop Elements; %d dead, %d dying skipped); will drain %zu/tick",
            g_snapshotCandidates.size(), peerSlot, trackedCount, skippedDead, skippedDying, kSnapshotChunkSize);

    // (The EAGER host chipPile death-watch enroll that used to live here was REMOVED 2026-06-17 with
    // the pile death-watch retirement. The host no longer needs to pre-enroll piles: a host grab is
    // now caught directly by the InpActEvt_use PRE observer reading lookAtActor=pile -> its eid (every
    // snapshot candidate is already element-bound, so GetPropElementIdForActor resolves it on grab),
    // so there is no drain-window race to pre-arm against. See trash_collect_sync.cpp.)

    g_snapshotSentTotal = 0;
    // v34: OPEN the joiner's loading-screen bracket. Send the candidate count (the progress
    // denominator) on Lane::Bulk BEFORE the first PropSpawn, so GNS in-lane ordering puts it
    // strictly ahead of the prop stream it introduces. Host-only path (TriggerForSlot gated
    // us to host + IsSlotReady).
    if (auto* s = g_session_ptr.load(std::memory_order_acquire)) {
        coop::net::SnapshotBeginPayload b{};
        b.propTotal = static_cast<uint32_t>(g_snapshotCandidates.size());
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::SnapshotBegin, &b, sizeof(b));
    }
}

// v34: finish the current drain -- CLOSE the joiner's loading-screen bracket, then clear
// state + dequeue the next pending slot. SnapshotComplete is the LAST Lane::Bulk message
// (after every PropSpawn) so the joiner hides its cover only once the whole stream landed.
// Single completion authority: called from DrainChunk's normal finish AND its 0-candidate
// early-out, so an empty world can't leave g_currentTargetSlot pinned (queue would stall).
// Shared drain-state clear -- used by the normal completion AND the fork-A
// mid-drain world-transition abort (which must NOT send SnapshotComplete).
void ClearDrainState_() {
    g_snapshotCandidates.clear(); g_snapshotCandidates.shrink_to_fit();
    g_snapshotEids.clear(); g_snapshotEids.shrink_to_fit();
    g_snapshotInternalIdxs.clear(); g_snapshotInternalIdxs.shrink_to_fit();
    g_snapshotCandidateIdx = 0;
    g_currentTargetSlot = -1;
    g_snapshotSentTotal = 0;
}

// Fork A: drain the pending queue through TriggerForSlot (the single gated
// entry point) until a drain starts or the queue empties. A not-ready slot
// is dropped (TriggerForSlot logs it), an incoherent-registry one moves to
// g_deferredSlots -- either way the LOOP continues, fixing the pre-existing
// stall where one dead dequeued slot abandoned the rest of the queue.
void DequeuePending_() {
    while (g_currentTargetSlot == -1 && !g_pendingSlots.empty()) {
        const int next = g_pendingSlots.front();
        g_pendingSlots.erase(g_pendingSlots.begin());
        TriggerForSlot(next);
    }
}

void CompleteDrainForCurrentSlot(coop::net::Session* s) {
    if (s && g_currentTargetSlot >= 1) {
        coop::net::SnapshotEndPayload e{};
        e.propSent = g_snapshotSentTotal;
        // Phase 0 (docs/COOP_STABLE_ID_SIDECAR.md S4 -- the docs/piles/10 over-destroy guard):
        // append a per-class completeness census (an INDEPENDENT GUObjectArray count of live
        // chipPiles, NOT the registry-driven snapshot enumeration) as a tail. The joiner's claim
        // sweep KEEPs a class whose claimed count is below the host's count = an incomplete snapshot.
        // No new ReliableKind (the receiver reads SnapshotEndPayload then the optional tail).
        std::vector<uint8_t> buf(sizeof(e));
        std::memcpy(buf.data(), &e, sizeof(e));
        std::vector<uint8_t> tail;
        const int budget = coop::net::kMaxReliablePayload - static_cast<int>(sizeof(e));
        coop::snapshot_census::BuildHostTail(tail, budget);
        buf.insert(buf.end(), tail.begin(), tail.end());
        s->SendReliableToSlot(g_currentTargetSlot, coop::net::ReliableKind::SnapshotComplete,
                              buf.data(), static_cast<int>(buf.size()));
    }
    UE_LOGI("snapshot: drain complete for slot %d (%zu candidates, %u sent)",
            g_currentTargetSlot, g_snapshotCandidates.size(), g_snapshotSentTotal);
    coop::dev::eid_lifetime_trace::EmitVerdict();  // S8.2: capture-eid vs wire-eid verdict (read-only)
    ClearDrainState_();
    // Dequeue the next pending slot(s) through the gated entry point (fresh
    // enumeration -- the world may have changed since this drain started).
    DequeuePending_();
}

// Build a PropSpawnPayload from a live actor + its eid/cached-index. THE single
// per-prop payload builder, shared by the bracketed drain (DrainChunk) AND the
// bracket-free incremental express (ExpressIncrementalSpawn) -- RULE 2: one
// builder, not two parallel copies. Returns false (caller skips the actor) when
// it is not expressible: null, dead, wire-suppressed, per-player, or
// unkeyed-and-not-a-chipPile. internalIdx >= 0 -> liveness re-checked via
// IsLiveByIndex (the drain's cached index, safe against a GC purge between
// enumerate and drain); < 0 -> the caller has already confirmed liveness (the
// incremental express). A keyless chipPile keeps key.len == 0 so the receiver
// routes it down the eidOnly lane (its cross-peer identity is the ElementId, not
// a key); anything else keyless is not expressible.
bool BuildPropSpawnPayload_(void* obj, coop::element::ElementId eid, int32_t internalIdx,
                            coop::net::PropSpawnPayload& p, int matchSlot) {
    if (!obj) return false;
    // Phase 0 PROOF (dev-only, ini-gated): inject the docs/piles/10 over-destroy by SKIPPING chipPile
    // expression, so the joiner's seeded natives stay UNCLAIMED -> its claim sweep dooms them en masse
    // (the floor binary KEEPs them; a no-floor baseline WIPES them). Host-only choke (this builder runs
    // only on the expressing host). No-op in any shipped config (the ini key is absent).
    if (coop::dev::force_overdestroy_test::HostSkipChipPileExpression() && ue_wrap::prop::IsChipPile(obj)) {
        return false;
    }
    // IsLiveByIndex (NOT IsLive) reads only the GUObjectArray slot, never obj's
    // (possibly freed) memory -- so a candidate GC-purged since enumeration is
    // rejected without faulting (the connect-edge crash root-caused 2026-05-30).
    if (internalIdx >= 0 && !R::IsLiveByIndex(obj, internalIdx)) return false;
    const std::wstring cls = R::ClassNameOf(obj);
    // Same wire-suppress allowlist + per-player skip as the Init POST observer:
    // intermediate-variant classes (mushroom7_C) and per-player props never
    // cross the wire (host-authoritative / each peer owns its own).
    if (coop::prop_lifecycle::IsWireSuppressedPropClass(cls)) return false;
    if (coop::prop_lifecycle::IsPerPlayerPropClass(cls)) return false;
    p.className.len = 0;
    for (size_t j = 0; j < cls.size() && j < 63; ++j) {
        p.className.data[p.className.len++] = static_cast<char>(cls[j]);
    }
    const std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(obj);
    if (keyStr.empty() || keyStr == L"None") {
        // Fork B HALF 1: a keyless chipPile IS expressible -- its cross-peer
        // identity is the ElementId (the receiver's eidOnly OnSpawn lane spawns +
        // watches it). Anything ELSE keyless has no stable cross-peer identity ->
        // not expressible (symmetric with the client sweep's universe test).
        if (!ue_wrap::prop::IsChipPile(obj) ||
            eid == coop::element::kInvalidId || eid == 0) {
            return false;
        }
        // p.key.len stays 0 -> the receiver routes down the eidOnly lane.
    } else {
        p.key.len = 0;
        for (size_t j = 0; j < keyStr.size() && j < 31; ++j) {
            p.key.data[p.key.len++] = static_cast<char>(keyStr[j]);
        }
    }
    // S8.2 eid-lifetime trace (read-only, host): this is the wire-eid the host is about to send for `obj`;
    // the trace compares it to the eid recorded for `obj` at save-capture (no-op unless [dev]
    // eid_lifetime_trace=1 AND obj was captured). Proves capture-eid == wire-eid before the bind is built.
    coop::dev::eid_lifetime_trace::CheckWireEid(obj, static_cast<uint32_t>(eid));
    const auto loc = ue_wrap::engine::GetActorLocation(obj);
    const auto rot = ue_wrap::engine::GetActorRotation(obj);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
    // v54: REAL scale -- part of the saved transform SP restores (loadData ->
    // SetActorScale3D); the old 1,1,1 hardcode mis-sized any scaled host prop.
    const auto scl = ue_wrap::engine::GetActorScale3D(obj);
    p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
    // SP-parity physics stamp (2026-06-09): kSimulatePhysics ONLY for an
    // actively-simulating Aprop_C (positively awake -- IsActorRootBodyAtRest
    // false -- so a settled-but-flag-awake prop mirrors kinematic at the host's
    // resting transform). non-Aprop_C keyed interactables (chipPile/trashBits/
    // clump) are kinematic by design -> physFlags=0. v54 also carries the raw
    // Static/sleep/removeWOrespawn bools + the list_props `Name` so the receiver's
    // init() builds the TRUE prop (mesh/mass/collision), not the CDO 'cube'.
    p.physFlags = 0;
    p.propName.len = 0;
    if (ue_wrap::prop::IsDescendantOfProp(obj)) {
        const bool isStatic = ue_wrap::prop::IsStatic(obj);
        const bool frozen   = ue_wrap::prop::IsFrozen(obj);
        const bool sleep    = ue_wrap::prop::IsSleeping(obj);
        if (!(isStatic || frozen || sleep) &&
            !ue_wrap::engine::IsActorRootBodyAtRest(obj)) {
            p.physFlags |= coop::net::propspawn_flags::kSimulatePhysics;
        }
        if (ue_wrap::prop::IsHeavy(obj)) p.physFlags |= coop::net::propspawn_flags::kIsHeavy;
        if (frozen)   p.physFlags |= coop::net::propspawn_flags::kFrozen;
        if (isStatic) p.physFlags |= coop::net::propspawn_flags::kStatic;
        if (sleep)    p.physFlags |= coop::net::propspawn_flags::kSleep;
        if (ue_wrap::prop::ReadRemoveWOrespawn(obj)) {
            p.physFlags |= coop::net::propspawn_flags::kRemoveWOrespawn;
        }
        const std::wstring nm = ue_wrap::prop::GetPropNameString(obj);
        for (size_t j = 0; j < nm.size() && j < 31; ++j) {
            p.propName.data[p.propName.len++] = static_cast<char>(nm[j]);
        }
    }
    p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
    p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;
    // Fork B HALF 1: carry the trash VARIANT (GetChipType is reflection-gated --
    // a no-op 0 for classes without a chipType property).
    p.chipType = ue_wrap::prop::GetChipType(obj);
    p.elementId = (eid == coop::element::kInvalidId) ? 0u : eid;
    // v86 Path 1c: for a chipPile in the JOIN snapshot (matchSlot = the target joiner), stamp its
    // SAVE-TIME position from the host's blob map so the client twin-destroy matches the save-loaded
    // native @old even if the host MOVED the pile @new in the join-load window (the two-channel DUP
    // fix). Only the join drain passes a real matchSlot; the mid-game incremental express passes -1
    // (no twin on the client -> no stamp). No map entry (unseeded/post-save pile) -> no stamp, the
    // receiver falls back to the current pose `loc`.
    p.hasMatchPos = 0;
    if (matchSlot >= 1 && p.elementId != 0 && ue_wrap::prop::IsChipPile(obj)) {
        ue_wrap::FVector sv;
        if (coop::save_transfer::TryGetSaveTimePileXform(matchSlot, p.elementId, sv)) {
            p.matchX = sv.X; p.matchY = sv.Y; p.matchZ = sv.Z;
            p.hasMatchPos = 1;
        }
    }
    return true;
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);
}

void TriggerForSlot(int peerSlot) {
    auto* s = g_session_ptr.load(std::memory_order_acquire);
    if (!s) return;
    if (s->role() != coop::net::Role::Host) {
        UE_LOGI("snapshot: not host -- skipping TriggerForSlot(slot=%d)", peerSlot);
        return;
    }
    if (peerSlot < 1 || peerSlot >= coop::players::kMaxPeers) {
        UE_LOGW("snapshot: TriggerForSlot peerSlot=%d out of [1..%u)",
                peerSlot, static_cast<unsigned>(coop::players::kMaxPeers));
        return;
    }
    // Fork A: consume the deferred flag (single owner -- whichever of the
    // net_pump re-seed retrigger / the DrainChunk flush runs first consumes
    // it; the gate below re-sets it if the registry is still incoherent).
    g_deferredSlots[peerSlot] = false;
    // Gate on IsSlotReady (lanes configured) rather than IsSlotConnected
    // (just has a connection handle). The Connecting status callback
    // sets peerConns_[slot] on host accept; that flips IsSlotConnected
    // true a few ms BEFORE the Connected callback runs
    // ConfigureLanesForPeer. Triggering the snapshot drain in that
    // window queues PropSpawn messages on the default lane 0 (HIGH)
    // instead of the BULK lane 2 -- defeating PR-3's head-of-line-block
    // mitigation for the worst case (initial ~1700-msg fan-out).
    // IsSlotReady fires only after lanes are live on the connection.
    if (!s->IsSlotReady(peerSlot)) {
        // Audit WARN-1 (2026-06-10): the deferred flag was consumed above --
        // restore it so the generation flush retries this slot once lanes
        // configure (the connect-edge retry is a rescue, not the contract).
        g_deferredSlots[peerSlot] = true;
        g_deferredSeenGen = PT::SeedGeneration();
        UE_LOGW("snapshot: slot %d not ready (lanes not yet configured) -- deferring TriggerForSlot", peerSlot);
        return;
    }
    // Fork A gate (2026-06-10, HARDENED after the smoke falsified the
    // stamp-only shape): a bracket is a DESTRUCTIVE contract -- the client
    // arms a claim set at SnapshotBegin and at SnapshotComplete destroys
    // every unclaimed in-universe local. It may only be built from a
    // registry that expresses the host's CURRENT world. Three signals, any
    // one defers:
    //  - !HasSeededOnce: the boot seed never ran -- nothing is expressed.
    //  - stamp dead: the stamped UWorld was GC-purged (a real world swap;
    //    O(1), zero detection latency).
    //  - InPurgeEpisode: the reaper detected a mass purge and the registry
    //    is draining a dead world's elements until the episode-end re-seed.
    //    REQUIRED in addition to the stamp: VOTV's boot/save-load flow can
    //    leave the stamped UWorld ALIVE while the registry is majority-dead
    //    (smoke 2026-06-10: stamp live, 1161 dying vs 88 live -> the
    //    stamp-only gate passed and the client swept 3067 actors against an
    //    88-prop bracket).
    // (A fourth, enumerate-time backstop lives in StartEnumerationFor for
    // the <=4s window before the reaper's next scan detects the purge.)
    // MTA gets this invariant for free (synchronous entity tree,
    // CMapManager::SendMapInformation); we restore it explicitly.
    if (!PT::HasSeededOnce() || !PT::IsRegistrySeededForCurrentWorld() ||
        PT::InPurgeEpisode()) {
        g_deferredSlots[peerSlot] = true;
        g_deferredSeenGen = PT::SeedGeneration();
        UE_LOGW("snapshot: registry does not express the current world (transition in progress) -- DEFERRING slot %d until the next re-seed", peerSlot);
        return;
    }
    if (g_currentTargetSlot != -1) {
        // Fork A: a re-trigger of the slot being drained RIGHT NOW, for the
        // SAME registry generation, is already covered by the in-flight
        // bracket -- queueing it would produce a duplicate identical bracket.
        if (peerSlot == g_currentTargetSlot && g_drainSeedGen == PT::SeedGeneration()) {
            return;
        }
        // Another drain is in flight. Queue this slot for after it
        // completes. Avoid duplicate queue entries (a slot reconnecting
        // multiple times in rapid succession).
        if (std::find(g_pendingSlots.begin(), g_pendingSlots.end(), peerSlot) ==
                g_pendingSlots.end()) {
            g_pendingSlots.push_back(peerSlot);
            UE_LOGI("snapshot: drain busy on slot %d -- queueing slot %d (depth=%zu)",
                    g_currentTargetSlot, peerSlot, g_pendingSlots.size());
        }
        return;
    }
    StartEnumerationFor(peerSlot);
}

void DrainChunk() {
    // Fork A deferred-slot flush: UNCONDITIONAL on coherence (independent of
    // the net_pump re-seed retrigger's added>0 condition) -- a deferred slot
    // retries on EVERY seed-generation bump. Level-triggered: TriggerForSlot
    // re-defers (re-latching the gen) if a second travel raced in. Runs
    // BEFORE the no-drain early-out (this is the only per-tick call site);
    // idle cost is a 3-bool scan.
    if (AnyDeferred_()) {
        const uint64_t gen = PT::SeedGeneration();
        if (gen != g_deferredSeenGen) {
            g_deferredSeenGen = gen;
            for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
                if (g_deferredSlots[slot]) TriggerForSlot(slot);  // consumes/re-sets its own flag
            }
        }
    }
    if (g_currentTargetSlot == -1) return;
    auto* s = g_session_ptr.load(std::memory_order_acquire);
    if (!s) return;
    // Bail out early if the target peer disconnected mid-drain. Without
    // this we'd iterate all remaining candidates calling SendReliableToSlot
    // into a closed connection (Session silently no-ops but the GetActor
    // Location/Rotation reflection calls add up to wasted ms/frame).
    if (!s->IsSlotConnected(g_currentTargetSlot)) {
        UE_LOGI("snapshot: target slot %d disconnected mid-drain -- aborting (sent %zu/%zu)",
                g_currentTargetSlot, g_snapshotCandidateIdx, g_snapshotCandidates.size());
        CancelForSlot(g_currentTargetSlot);
        return;
    }
    // Fork A mid-drain abort: the host travelled while this bracket was
    // streaming. Every remaining candidate is dying and the per-candidate
    // IsLiveByIndex below would rush the bracket to a SnapshotComplete with
    // PARTIAL claims -> the client sweep would mass-destroy. Abort WITHOUT
    // SnapshotComplete (the sweep only fires on Complete; the client's armed
    // claim set is membership-only and the re-bracket's SnapshotBegin clears
    // + re-arms it) and defer the slot for the post-re-seed re-bracket.
    // (Episode check included: the reaper can detect the purge mid-drain.)
    if (!PT::IsRegistrySeededForCurrentWorld() || PT::InPurgeEpisode()) {
        UE_LOGW("snapshot: world transition mid-drain (slot %d, %u sent) -- aborting WITHOUT SnapshotComplete; slot deferred",
                g_currentTargetSlot, g_snapshotSentTotal);
        g_deferredSlots[g_currentTargetSlot] = true;
        g_deferredSeenGen = PT::SeedGeneration();
        ClearDrainState_();
        DequeuePending_();
        return;
    }
    // v34: nothing to drain (0 candidates, or already drained) -> complete now so the
    // SnapshotComplete bracket closes + the pending queue advances. A 0-candidate world
    // previously bare-returned here and pinned g_currentTargetSlot forever.
    if (g_snapshotCandidateIdx >= g_snapshotCandidates.size()) {
        CompleteDrainForCurrentSlot(s);
        return;
    }
    // (std::min) parenthesized to defeat windows.h's `min` macro.
    const size_t limit = (std::min)(g_snapshotCandidateIdx + kSnapshotChunkSize,
                                    g_snapshotCandidates.size());
    // (v15 hoisted a paired LocalPlayerIdentity read above the loop to
    // stamp p.senderContext on every snapshot packet; v16 PR-FOUNDATION-1b
    // moved per-peer stale-gen defense to the packet header senderEpoch,
    // stamped uniformly by Session::SendReliableToSlot via WriteHeader.
    // No per-packet sender state needed at the gameplay layer.)
    int sent = 0;
    for (; g_snapshotCandidateIdx < limit; ++g_snapshotCandidateIdx) {
        void* obj = g_snapshotCandidates[g_snapshotCandidateIdx];
        // Re-validate liveness: an actor live at enumeration may have been
        // GC-purged across the intervening ticks. IsLiveByIndex with the cached
        // index reads only the GUObjectArray slot, never obj's (possibly freed)
        // memory -- so a purged candidate is skipped without faulting. Once it
        // passes, obj is live and the subsequent UFunction reads below are safe
        // (UE4 GC purge runs on the game thread and cannot interleave mid-tick).
        const int32_t cachedIdx = g_snapshotCandidateIdx < g_snapshotInternalIdxs.size()
                                      ? g_snapshotInternalIdxs[g_snapshotCandidateIdx]
                                      : -1;
        if (!obj || !R::IsLiveByIndex(obj, cachedIdx)) continue;
        // Cached eid from StartEnumerationFor (no per-candidate prop-tracker
        // mutex; audit fix 2026-05-28).
        const coop::element::ElementId eid =
            g_snapshotCandidateIdx < g_snapshotEids.size()
                ? g_snapshotEids[g_snapshotCandidateIdx]
                : coop::element::kInvalidId;
        coop::net::PropSpawnPayload p{};
        // Build via the shared per-prop builder (keyed vs keyless/eid-only pile
        // handling, wire-suppress + per-player skips, v54 physics + identity). A
        // non-expressible candidate returns false -> skip. cachedIdx already
        // re-validated liveness above, so pass -1 (don't repeat IsLiveByIndex).
        // v86 Path 1c: this is the JOIN drain -- pass the target joiner slot so a pile gets its
        // save-time match key stamped (g_currentTargetSlot is this drain's single target).
        if (!BuildPropSpawnPayload_(obj, eid, -1, p, g_currentTargetSlot)) continue;
        // Send to ONE slot only. Other peers (already-connected) already
        // have these props from their own connect-edge drain.
        s->SendReliableToSlot(g_currentTargetSlot,
                              coop::net::ReliableKind::PropSpawn,
                              &p, sizeof(p));
        ++sent;
    }
    g_snapshotSentTotal += static_cast<uint32_t>(sent);
    if (g_snapshotCandidateIdx >= g_snapshotCandidates.size()) {
        CompleteDrainForCurrentSlot(s);  // sends SnapshotComplete + clears + dequeues next
    } else {
        UE_LOGI("snapshot: drained chunk for slot %d -- this tick=%d, processed %zu/%zu",
                g_currentTargetSlot, sent, g_snapshotCandidateIdx, g_snapshotCandidates.size());
    }
}

void ExpressIncrementalSpawn(void* actor) {
    auto* s = g_session_ptr.load(std::memory_order_acquire);
    // Host-authoritative: only the host broadcasts world spawns. The steady-world
    // re-seed still runs on a client (it mints the client's OWN local eids) but
    // never broadcasts -- a client's runtime spawns route through the host (phase 2).
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!actor || !R::IsLive(actor)) return;
    // Kerfur exclusion (2026-06-18 backstop to the MarkKnownKeyedProp close in
    // RegisterHostPropSilent): a kerfur's ONLY wire signal is KerfurConvert -- never the
    // generic incremental PropSpawn. Re-expressing a kerfur prop here with its real BP key
    // dupes it on a client fuzzy-miss (the host turn-on/off dupe). The connect snapshot's
    // drain handles a kerfur prop differently (the client adopts it); only THIS steady-state
    // express must skip it.
    if (coop::kerfur_entity::IsKerfurActor(actor)) return;
    // eid was minted by the seed walk that yielded this actor (phase 2 of
    // SeedWalk_, before it returned), so this resolves.
    const coop::element::ElementId eid = PT::GetPropElementIdForActor(actor);
    coop::net::PropSpawnPayload p{};
    // internalIdx -1: liveness just confirmed above (IsLive); the builder's idx
    // guard is only for the drain's cached-index path.
    // v86 Path 1c: mid-game incremental express (NOT a join) -- matchSlot=-1, no save-time stamp
    // (a runtime-spawned pile reaching a long-connected peer has no save-loaded native twin).
    if (!BuildPropSpawnPayload_(actor, eid, -1, p, -1)) return;  // not expressible -- skip silently
    // Broadcast ONE additive PropSpawn to all ready peers (MTA CEntityAddPacket /
    // BroadcastOnlyJoined; the SAME s->SendPropSpawn the Init-POST observer uses for
    // an organically-spawned prop). NO SnapshotBegin/Complete bracket -> the client's
    // destructive divergence sweep is NOT re-armed (the entire point of R1). A peer
    // that also enumerates this prop in its own in-flight connect drain just dedupes
    // it via RegisterPropMirror.
    s->SendPropSpawn(p);
    UE_LOGI("snapshot: incremental PropSpawn for runtime-adopted prop %p (eid=%u, key='%.*s') "
            "-- bracket-free additive add (MTA CEntityAddPacket; no sweep re-arm)",
            actor, p.elementId, static_cast<int>(p.key.len), p.key.data);
}

void CancelForSlot(int peerSlot) {
    // Fork A: a slot that disconnects while deferred must not be retried by
    // the flush. Clear BEFORE the in-progress check (the flag is independent
    // of whether this slot was draining).
    if (peerSlot >= 1 && peerSlot < coop::players::kMaxPeers) {
        g_deferredSlots[peerSlot] = false;
    }
    // Remove `peerSlot` from the pending queue (may have queued + then
    // disconnected before its turn came up).
    g_pendingSlots.erase(
        std::remove(g_pendingSlots.begin(), g_pendingSlots.end(), peerSlot),
        g_pendingSlots.end());
    // If `peerSlot` was the in-progress drain target, abort + dequeue next.
    if (g_currentTargetSlot != peerSlot) return;
    ClearDrainState_();
    DequeuePending_();
}

size_t OnDisconnect() {
    size_t pending = 0;
    if (!g_snapshotCandidates.empty()) {
        pending = g_snapshotCandidates.size() - g_snapshotCandidateIdx;
    }
    ClearDrainState_();
    g_pendingSlots.clear();
    g_pendingSlots.shrink_to_fit();
    // Fork A: no deferred slot may survive into the next session.
    g_deferredSlots.fill(false);
    g_deferredSeenGen = 0;
    g_drainSeedGen = 0;
    return pending;
}

}  // namespace coop::prop_snapshot
