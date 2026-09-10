// coop/props/prop_snapshot.cpp -- the host's prop snapshot to a joining peer: one slot at a time
// through Session::SendReliableToSlot (a second peer connecting mid-drain queues), chunked per
// tick, bracketed by SnapshotBegin and SnapshotComplete; plus the bracket-free incremental express
// for a prop adopted into tracking after the join. Interface: coop/props/prop_snapshot.h.

#include "coop/props/prop_snapshot.h"

#include "coop/config/config.h"  // ReadEnv, the dedupe-bypass drill
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/creatures/kerfur_convert.h"  // TryAdoptFreshKerfurProp, the builder's kerfur first refusal
#include "coop/creatures/kerfur_entity.h"  // IsKerfurActor; a kerfur's only signal is KerfurConvert
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_save_data.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_lifecycle.h"
#include "coop/props/remote_prop.h"
#include "coop/save/save_transfer.h"  // TryGetSaveTimePileXform, the join snapshot's pile match key
#include "coop/props/snapshot_census.h"  // the per-class completeness census on SnapshotComplete
#include "coop/dev/eid_lifetime_trace.h"  // read-only: capture-eid vs wire-eid
#include "coop/dev/force_overdestroy_test.h"  // dev only: inject the over-destroy to prove the floor
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

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

// Atomic: SetSession runs off the game thread at boot (the loader thread), DrainChunk and
// TriggerForSlot on it.
std::atomic<coop::net::Session*> g_session_ptr{nullptr};

// The running drain's enumeration, game thread only.
std::vector<void*> g_snapshotCandidates;
std::vector<coop::element::ElementId> g_snapshotEids;  // parallel; same idx as g_snapshotCandidates
// Each candidate's GUObjectArray index, captured while it was live: DrainChunk re-validates with
// IsLiveByIndex, so a candidate purged between enumeration and its tick is rejected without a
// dereference.
std::vector<int32_t> g_snapshotInternalIdxs;
size_t g_snapshotCandidateIdx = 0;
// PropSpawns actually sent this drain (skips excluded); reported in SnapshotComplete.
uint32_t g_snapshotSentTotal = 0;
// The slot being drained (-1 = none) and the slots waiting their turn.
int g_currentTargetSlot = -1;
std::vector<int> g_pendingSlots;

// Slots whose bracket was deferred because the registry does not yet express the host's current
// world (mid world-transition). The flush at the top of DrainChunk retries them on every
// seed-generation bump; TriggerForSlot consumes, and may re-set, the flag. Game thread.
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

// 100 candidates per tick keeps the per-tick reflection cost well inside a frame; ~2,000 props
// drain over 20-30 ticks with no single-frame stall.
constexpr size_t kSnapshotChunkSize = 100;

// Enumerates the candidates from the element registry (a pointer copy under its mutex, not an
// array walk), sets the target slot and opens the bracket. The caller ensures no drain is in
// progress. Per-candidate liveness is re-checked in DrainChunk.
void StartEnumerationFor(int peerSlot) {
    g_currentTargetSlot = peerSlot;
    // The generation this drain expresses: TriggerForSlot dedupes a re-trigger of the same slot for
    // the same generation.
    g_drainSeedGen = PT::SeedGeneration();
    g_snapshotCandidates.clear();
    g_snapshotEids.clear();
    g_snapshotInternalIdxs.clear();
    g_snapshotCandidateIdx = 0;
    int skippedDying = 0, skippedDead = 0;
    // SnapshotActorsByType copies (actor, id, index) under the registry mutex; no Element is
    // dereferenced after it releases, and the eid travels with the actor so the drain takes no
    // per-candidate lock.
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    const size_t trackedCount =
        coop::element::Registry::Get().SnapshotActorsByType(
            coop::element::ElementType::Prop, pairs);
    g_snapshotCandidates.reserve(trackedCount);
    g_snapshotEids.reserve(trackedCount);
    g_snapshotInternalIdxs.reserve(trackedCount);
    for (const auto& pr : pairs) {
        if (!pr.actor) { ++skippedDead; continue; }
        // IsLiveByIndex: a mass purge does not fire K2_DestroyActor, so dying props linger in the
        // registry, and IsLive on the freed pointer faults. Only the GUObjectArray slot is read.
        if (!R::IsLiveByIndex(pr.actor, pr.internalIdx)) { ++skippedDying; continue; }
        g_snapshotCandidates.push_back(pr.actor);
        g_snapshotEids.push_back(pr.id);
        g_snapshotInternalIdxs.push_back(pr.internalIdx);
    }
    // A registry whose dying elements outnumber its live ones is mid-purge, not a live world: this
    // closes the window (up to 4 s) between a world transition and the reaper's next scan raising
    // the purge-episode flag. In steady gameplay dying is ~0 (normal destruction is evicted inline)
    // and a sublevel stream-out is small against the live world, so majority-dead only means a
    // transition.
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

    g_snapshotSentTotal = 0;
    // SnapshotBegin carries the candidate count (the progress denominator) on the bulk lane ahead
    // of the first PropSpawn; in-lane ordering keeps it first. Host only.
    if (auto* s = g_session_ptr.load(std::memory_order_acquire)) {
        coop::net::SnapshotBeginPayload b{};
        b.propTotal = static_cast<uint32_t>(g_snapshotCandidates.size());
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::SnapshotBegin, &b, sizeof(b));
    }
}

// The shared drain-state clear: the normal completion and the mid-drain world-transition abort
// (which must not send SnapshotComplete).
void ClearDrainState_() {
    g_snapshotCandidates.clear(); g_snapshotCandidates.shrink_to_fit();
    g_snapshotEids.clear(); g_snapshotEids.shrink_to_fit();
    g_snapshotInternalIdxs.clear(); g_snapshotInternalIdxs.shrink_to_fit();
    g_snapshotCandidateIdx = 0;
    g_currentTargetSlot = -1;
    g_snapshotSentTotal = 0;
}

// Dequeues through TriggerForSlot until a drain starts or the queue empties: a not-ready slot is
// dropped, an incoherent-registry one defers, and the loop goes on, so one dead slot cannot stall
// the rest of the queue.
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
        // SnapshotComplete closes the bracket as the last bulk-lane message, so the joiner lifts
        // its cover only once the whole stream landed. Its tail is the per-class completeness
        // census, an independent GUObjectArray count of live chipPiles (not the registry
        // enumeration); the joiner's sweep keeps a class it claimed fewer of than the host
        // reported. No new ReliableKind: the receiver reads the payload, then the optional tail.
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
    coop::dev::eid_lifetime_trace::EmitVerdict();  // read-only: capture-eid vs wire-eid verdict
    ClearDrainState_();
    // The next pending slot re-enumerates: the world may have changed since this drain started.
    DequeuePending_();
}

// The one PropSpawnPayload builder, shared by the bracketed drain and the incremental express.
// False when the actor is not expressible: null, dead, wire-suppressed, per-player, or keyless and
// not a chipPile. internalIdx >= 0 re-checks liveness by index (the drain's cached index); < 0
// means the caller confirmed it. A keyless chipPile keeps key.len 0, so the receiver takes the
// eid-only lane.
bool BuildPropSpawnPayload_(void* obj, coop::element::ElementId eid, int32_t internalIdx,
                            coop::net::PropSpawnPayload& p, int matchSlot) {
    if (!obj) return false;
    // A child actor (a kerfur's eye cam) is never expressed: without MarkPropElement it would go
    // out as a keyed payload with elementId 0 and re-create a floating mirror the gated destroy
    // seam never tears down. Predicate and rationale: ue_wrap::engine::IsChildActor.
    if (ue_wrap::engine::IsChildActor(obj)) return false;
    // Dev only, ini-gated: skipping chipPile expression leaves the joiner's natives unclaimed, so
    // its sweep would doom them en masse and the completeness floor must keep them. A no-op without
    // the ini key.
    if (coop::dev::force_overdestroy_test::HostSkipChipPileExpression() && ue_wrap::prop::IsChipPile(obj)) {
        return false;
    }
    // By index: a candidate purged since enumeration is rejected without touching freed memory.
    if (internalIdx >= 0 && !R::IsLiveByIndex(obj, internalIdx)) return false;
    const std::wstring cls = R::ClassNameOf(obj);
    // The same wire-suppress and per-player skips as the Init observer: an intermediate variant
    // (mushroom7_C) and a per-player prop never cross the wire.
    if (coop::prop_lifecycle::IsWireSuppressedPropClass(cls)) return false;
    if (coop::prop_lifecycle::IsPerPlayerPropClass(cls)) return false;
    // The kerfur first refusal, the twin of the one in prop_lifecycle's express: an untracked
    // kerfur prop reaching this builder is adopted by KerfurConvert, which broadcasts instead.
    // Every actor the drain offers is registry-fed and tracked, so it exits at TryAdopt's
    // untracked-only guard; the gate covers any future lane that feeds an untracked kerfur prop
    // here.
    if (coop::kerfur_entity::IsKerfurPropClass(R::ClassOf(obj)) &&
        coop::kerfur_convert::TryAdoptFreshKerfurProp(obj)) {
        return false;  // converged: KerfurConvert broadcast, no generic payload
    }
    p.className.len = 0;
    for (size_t j = 0; j < cls.size() && j < 63; ++j) {
        p.className.data[p.className.len++] = static_cast<char>(cls[j]);
    }
    const std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(obj);
    if (keyStr.empty() || keyStr == L"None") {
        // A keyless chipPile is expressible: its cross-peer identity is the eid, and the receiver's
        // eid-only lane spawns it. Anything else keyless has no stable identity, matching the
        // client sweep's universe test.
        if (!ue_wrap::prop::IsChipPile(obj) ||
            eid == coop::element::kInvalidId || eid == 0) {
            return false;
        }
        // key.len stays 0: the eid-only lane.
    } else {
        p.key.len = 0;
        for (size_t j = 0; j < keyStr.size() && j < 31; ++j) {
            p.key.data[p.key.len++] = static_cast<char>(keyStr[j]);
        }
    }
    // Read-only trace: the wire eid against the eid recorded at save capture; a no-op unless
    // enabled.
    coop::dev::eid_lifetime_trace::CheckWireEid(obj, static_cast<uint32_t>(eid));
    const auto loc = ue_wrap::engine::GetActorLocation(obj);
    // A chipPile's visual variety is the StaticMesh component's relative rotation (a random roll
    // from the construction script), not the actor root, and the trash mirror is a bare
    // AStaticMeshActor: the visible mesh's world rotation goes on the wire, or every proxy pile
    // renders identically oriented. A native keyed prop keeps its actor rotation.
    const auto rot = ue_wrap::prop::IsChipPile(obj)
                         ? ue_wrap::engine::GetVisibleMeshWorldRotation(obj)
                         : ue_wrap::engine::GetActorRotation(obj);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
    // The real scale, part of the saved transform single-player restores.
    const auto scl = ue_wrap::engine::GetActorScale3D(obj);
    p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
    // kSimulatePhysics only for an Aprop_C that is positively awake (not static, frozen or asleep,
    // and its root body not at rest), so a settled prop mirrors kinematic at the host's resting
    // transform; a non-Aprop_C interactable is kinematic by design. The raw Static, frozen, sleep
    // and removeWOrespawn bits and the list_props Name go too, so the receiver's init builds the
    // real prop (mesh, mass, collision), not the CDO cube.
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
    // The trash variant; GetChipType is 0 for a class without the property.
    p.chipType = ue_wrap::prop::GetChipType(obj);
    p.elementId = (eid == coop::element::kInvalidId) ? 0u : eid;
    // For a chipPile in a join snapshot (matchSlot is the joiner), the save-time position from the
    // host's blob map, so the client's twin destroy matches the native loaded at the old spot even
    // if the host moved the pile in the join-load window. The incremental express passes -1 (no
    // twin on the client); no map entry, no stamp, and the receiver falls back to the live pose.
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
    // The deferred flag is consumed here, by whichever of the re-seed retrigger and the DrainChunk
    // flush runs first; the gates below re-set it.
    g_deferredSlots[peerSlot] = false;
    // IsSlotReady, not IsSlotConnected: the connection handle exists a few ms before the lanes are
    // configured, and a drain triggered in that window queues ~1,700 PropSpawns on the
    // high-priority lane instead of the bulk lane.
    if (!s->IsSlotReady(peerSlot)) {
        // The flag goes back so the generation flush retries once the lanes configure.
        g_deferredSlots[peerSlot] = true;
        g_deferredSeenGen = PT::SeedGeneration();
        UE_LOGW("snapshot: slot %d not ready (lanes not yet configured) -- deferring TriggerForSlot", peerSlot);
        return;
    }
    // A bracket is a destructive contract (the client destroys every unclaimed in-universe local at
    // SnapshotComplete), so it may only be built from a registry that expresses the host's current
    // world. Any of three signals defers: the boot seed never ran; the stamped UWorld was purged (a
    // real world swap); the reaper detected a mass purge and the registry is draining a dead world.
    // The third is needed beside the stamp: a save load can leave the stamped UWorld alive while
    // the registry is majority-dead, and the stamp alone once let a client sweep its world against
    // an 88-prop bracket. A fourth, enumerate-time backstop sits in StartEnumerationFor. MTA has
    // this invariant for free from its synchronous entity tree (CMapManager::SendMapInformation).
    if (!PT::HasSeededOnce() || !PT::IsRegistrySeededForCurrentWorld() ||
        PT::InPurgeEpisode()) {
        g_deferredSlots[peerSlot] = true;
        g_deferredSeenGen = PT::SeedGeneration();
        UE_LOGW("snapshot: registry does not express the current world (transition in progress) -- DEFERRING slot %d until the next re-seed", peerSlot);
        return;
    }
    if (g_currentTargetSlot != -1) {
        // A re-trigger of the slot being drained now, for the same generation, is covered by the
        // in-flight bracket; queueing it would send a duplicate bracket. The env bypass is a drill:
        // it must turn the zero-duplicate-brackets check red.
        static const bool sDedupBypass =
            !coop::config::ReadEnv("VOTVCOOP_SNAPSHOT_DEDUP_BYPASS").empty();
        if (!sDedupBypass &&
            peerSlot == g_currentTargetSlot && g_drainSeedGen == PT::SeedGeneration()) {
            return;
        }
        // Another drain is in flight: queue this slot, once.
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
    // The deferred-slot flush: every seed-generation bump retries each deferred slot, and
    // TriggerForSlot re-defers if a second travel raced in. Before the no-drain early-out (the only
    // per-tick call site); idle cost is a scan of the flags.
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
    // A target that disconnected mid-drain aborts now, or every remaining candidate pays its
    // reflection reads to send into a closed connection.
    if (!s->IsSlotConnected(g_currentTargetSlot)) {
        UE_LOGI("snapshot: target slot %d disconnected mid-drain -- aborting (sent %zu/%zu)",
                g_currentTargetSlot, g_snapshotCandidateIdx, g_snapshotCandidates.size());
        CancelForSlot(g_currentTargetSlot);
        return;
    }
    // The host travelled mid-drain: every remaining candidate is dying, and the per-candidate
    // liveness check would rush the bracket to a SnapshotComplete with partial claims, a mass
    // destroy on the client. Abort without SnapshotComplete (the sweep fires only on Complete; the
    // re-bracket's Begin clears and re-arms the claim set) and defer the slot for the post-re-seed
    // bracket.
    if (!PT::IsRegistrySeededForCurrentWorld() || PT::InPurgeEpisode()) {
        UE_LOGW("snapshot: world transition mid-drain (slot %d, %u sent) -- aborting WITHOUT SnapshotComplete; slot deferred",
                g_currentTargetSlot, g_snapshotSentTotal);
        g_deferredSlots[g_currentTargetSlot] = true;
        g_deferredSeenGen = PT::SeedGeneration();
        ClearDrainState_();
        DequeuePending_();
        return;
    }
    // Nothing left (or nothing at all) completes now, so the bracket closes and the queue advances;
    // an empty world once pinned the target slot forever.
    if (g_snapshotCandidateIdx >= g_snapshotCandidates.size()) {
        CompleteDrainForCurrentSlot(s);
        return;
    }
    // (std::min) parenthesised against windows.h's macro.
    const size_t limit = (std::min)(g_snapshotCandidateIdx + kSnapshotChunkSize,
                                    g_snapshotCandidates.size());
    int sent = 0;
    for (; g_snapshotCandidateIdx < limit; ++g_snapshotCandidateIdx) {
        void* obj = g_snapshotCandidates[g_snapshotCandidateIdx];
        // Liveness re-validated by the cached index (a purge since enumeration); once it passes,
        // the UFunction reads below are safe, since the GC purge runs on the game thread and cannot
        // interleave mid-tick.
        const int32_t cachedIdx = g_snapshotCandidateIdx < g_snapshotInternalIdxs.size()
                                      ? g_snapshotInternalIdxs[g_snapshotCandidateIdx]
                                      : -1;
        if (!obj || !R::IsLiveByIndex(obj, cachedIdx)) continue;
        // The eid cached at enumeration.
        const coop::element::ElementId eid =
            g_snapshotCandidateIdx < g_snapshotEids.size()
                ? g_snapshotEids[g_snapshotCandidateIdx]
                : coop::element::kInvalidId;
        coop::net::PropSpawnPayload p{};
        // -1: liveness was just re-validated. The target slot makes this the join drain, so a pile
        // gets its save-time match key.
        if (!BuildPropSpawnPayload_(obj, eid, -1, p, g_currentTargetSlot)) continue;
        // To the one slot: the other peers got these props from their own drain.
        s->SendReliableToSlot(g_currentTargetSlot,
                              coop::net::ReliableKind::PropSpawn,
                              &p, sizeof(p));
        // The prop's own save record to the same slot, behind its spawn row: a prop whose state
        // changed after the transferred save was written is not in that save.
        coop::prop_save_data::PublishWithSpawn(s, obj, p.key, g_currentTargetSlot);
        ++sent;
    }
    g_snapshotSentTotal += static_cast<uint32_t>(sent);
    if (g_snapshotCandidateIdx >= g_snapshotCandidates.size()) {
        CompleteDrainForCurrentSlot(s);  // SnapshotComplete, clear, dequeue the next
    } else {
        UE_LOGI("snapshot: drained chunk for slot %d -- this tick=%d, processed %zu/%zu",
                g_currentTargetSlot, sent, g_snapshotCandidateIdx, g_snapshotCandidates.size());
    }
}

// One additive PropSpawn for `actor` to every ready peer, with no bracket, so the client's
// divergence sweep is not re-armed (MTA's CEntityAddPacket shape). A peer that also enumerates
// this prop in its own in-flight drain dedupes it in RegisterPropMirror. `s` is host-validated and
// `actor` live, by the caller. kindTag is a log prefix.
static void BroadcastIncrementalPropSpawn_(coop::net::Session* s, void* actor, const char* kindTag) {
    // The eid was minted by the seed walk that yielded this actor.
    const coop::element::ElementId eid = PT::GetPropElementIdForActor(actor);
    coop::net::PropSpawnPayload p{};
    // -1 and -1: liveness confirmed by the caller, and a mid-game express has no save-loaded twin
    // to stamp a match key for.
    if (!BuildPropSpawnPayload_(actor, eid, -1, p, -1)) return;  // not expressible
    s->SendPropSpawn(p);
    coop::prop_save_data::PublishWithSpawn(s, actor, p.key);
    UE_LOGI("snapshot: incremental PropSpawn for runtime-adopted %sprop %p (eid=%u, key='%.*s') "
            "-- bracket-free additive add (MTA CEntityAddPacket; no sweep re-arm)",
            kindTag, actor, p.elementId, static_cast<int>(p.key.len), p.key.data);
}

bool ExpressWouldBroadcast() {
    auto* s = g_session_ptr.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

void ExpressIncrementalSpawn(void* actor) {
    auto* s = g_session_ptr.load(std::memory_order_acquire);
    // Only the host broadcasts world spawns; a client's re-seed still runs but never broadcasts,
    // and its runtime spawns route through the host.
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!actor || !R::IsLive(actor)) return;
    // A kerfur's only steady-state wire signal is KerfurConvert: re-expressing a converted kerfur
    // prop here with its real key dupes it on a client fuzzy miss. A kerfur off-prop no convert
    // delivered (the join-window registration race) goes through ExpressIncrementalKerfurOffProp
    // instead, from the same re-seed loop.
    if (coop::kerfur_entity::IsKerfurActor(actor)) return;
    BroadcastIncrementalPropSpawn_(s, actor, /*kindTag=*/"");
}

void ExpressIncrementalKerfurOffProp(void* actor) {
    auto* s = g_session_ptr.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!actor || !R::IsLive(actor)) return;
    // Only kerfur actors are routed here, and a re-seed-new kerfur is always the off-prop form (the
    // re-seed adopts keyed props, never the active NPC).
    if (!coop::kerfur_entity::IsKerfurActor(actor)) return;
    // Only an un-converted off-prop is delivered here. A converted one has a KerfurId bound to its
    // eid and KerfurConvert owns it; the MarkKnownKeyedProp discriminator keeps it out of the
    // re-seed-new set, so a KerfurId-bound eid here is a regression. The kerfur-delivery autotest
    // requires zero of this WARN.
    {
        const coop::element::ElementId checkEid = PT::GetPropElementIdForActor(actor);
        if (coop::kerfur_entity::GetKerfurIdForEid(checkEid) != coop::element::kInvalidId) {
            UE_LOGW("prop_snapshot: kerfur-off deliver-missing owner SKIPPED a convert-owned kerfur eid=%u "
                    "(KerfurId-bound -> KerfurConvert owns it) -- OWNER BOUNDARY violation (a converted off-prop "
                    "leaked into the re-seed-NEW set; the MarkKnownKeyedProp discriminator regressed)",
                    static_cast<uint32_t>(checkEid));
            return;
        }
    }
    // The join-edge deliver-missing owner. A kerfur off-prop created by a host turn-off during a
    // client's join window has no other channel: it post-dates the snapshot, the generic express
    // skips kerfurs, and the death-watch never fired a KerfurConvert (its NPC was not yet a watched
    // element; the host registers world NPCs once at join, and the turn-off can race that scan). A
    // converted off-prop is marked known before it can surface as re-seed-new, so what arrives here
    // is un-converted and never double-delivered. Dupe-safe in every ordering: the client applies
    // it through kerfur_prop_adoption::Arm, which no-ops an already-bound eid, and the wire eid is
    // the host's Prop element eid, so a later KerfurConvert for the same actor dedupes by it. Join
    // edge only: in steady state the death-watch is reliable and KerfurConvert stays primary; this
    // owner must not become a steady-state channel without a periodic reconcile behind it.
    BroadcastIncrementalPropSpawn_(s, actor, /*kindTag=*/"kerfur-off ");
}

void DeliverLateRegisteredProps(const std::vector<void*>& lateProps) {
    // The host-side late-registration owner: the steady-world re-seed hands over every prop it
    // newly adopted into tracking, one no fast channel (the snapshot, the Init express,
    // KerfurConvert) had delivered; each goes out once, and the client's idempotent apply absorbs
    // any overlap. So on the join edge correctness no longer depends on which channel caught a
    // window mutation. A generic prop takes the incremental PropSpawn; a kerfur off-prop the
    // deferred-adoption path.
    for (void* a : lateProps) {
        if (coop::kerfur_entity::IsKerfurActor(a)) ExpressIncrementalKerfurOffProp(a);
        else                                       ExpressIncrementalSpawn(a);
    }
}

void CancelForSlot(int peerSlot) {
    // A slot that disconnects while deferred is not retried; cleared before the in-progress check.
    if (peerSlot >= 1 && peerSlot < coop::players::kMaxPeers) {
        g_deferredSlots[peerSlot] = false;
    }
    // Out of the pending queue (queued, then disconnected before its turn).
    g_pendingSlots.erase(
        std::remove(g_pendingSlots.begin(), g_pendingSlots.end(), peerSlot),
        g_pendingSlots.end());
    // The in-progress target aborts and the next slot dequeues.
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
    // No deferred slot survives into the next session.
    g_deferredSlots.fill(false);
    g_deferredSeenGen = 0;
    g_drainSeedGen = 0;
    return pending;
}

}  // namespace coop::prop_snapshot
