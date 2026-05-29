// coop/prop_snapshot.cpp -- Phase 5S0 save snapshot bootstrap.
//
// Per-slot drain: snapshot replays to ONE peer slot at a time via
// Session::SendReliableToSlot. Late-joiners get a full snapshot;
// concurrent late-joiners don't race on a shared candidate index.
// Serial drains: if a second peer connects mid-drain, it queues and
// drains after the first completes.
// See coop/prop_snapshot.h for the public interface.

#include "coop/prop_snapshot.h"

#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/players_registry.h"
#include "coop/prop_lifecycle.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::prop_snapshot {
namespace {

namespace R = ue_wrap::reflection;

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
size_t g_snapshotCandidateIdx = 0;

// Which peer slot is being served by the current drain (-1 = no drain
// in progress). Plus the queue of slots waiting their turn (e.g. peer 2
// connecting while peer 1's drain is still in flight).
int g_currentTargetSlot = -1;
std::vector<int> g_pendingSlots;

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
    g_snapshotCandidates.clear();
    g_snapshotEids.clear();
    g_snapshotCandidateIdx = 0;
    int skippedDying = 0, skippedDead = 0;
    // Tier 3 Props migration 2026-05-28: enumerate via the unified Element
    // Registry instead of prop_lifecycle's legacy g_knownKeyedProps set.
    // SnapshotActorsByType extracts (actor*, id) pairs under the Registry
    // mutex -- no Element* dereferences happen after the mutex releases,
    // so no use-after-free if another thread frees an Element concurrently.
    //
    // Audit fix 2026-05-28: capture the eid alongside the actor pointer so
    // DrainChunk doesn't re-lock g_propElementsMutex per candidate
    // (12,500 mutex acquisitions/sec during drain was the prior cost).
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    const size_t trackedCount =
        coop::element::Registry::Get().SnapshotActorsByType(
            coop::element::ElementType::Prop, pairs);
    g_snapshotCandidates.reserve(trackedCount);
    g_snapshotEids.reserve(trackedCount);
    for (const auto& pr : pairs) {
        if (!pr.actor) { ++skippedDead; continue; }
        if (!R::IsLive(pr.actor)) { ++skippedDying; continue; }
        g_snapshotCandidates.push_back(pr.actor);
        g_snapshotEids.push_back(pr.id);
    }
    UE_LOGI("snapshot: enumerated %zu live candidates for slot %d from element::Registry (%zu Prop Elements; %d dead, %d dying skipped); will drain %zu/tick",
            g_snapshotCandidates.size(), peerSlot, trackedCount, skippedDead, skippedDying, kSnapshotChunkSize);
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
        UE_LOGW("snapshot: slot %d not ready (lanes not yet configured) -- skipping TriggerForSlot", peerSlot);
        return;
    }
    if (g_currentTargetSlot != -1) {
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
    if (g_currentTargetSlot == -1) return;
    if (g_snapshotCandidateIdx >= g_snapshotCandidates.size()) return;
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
    // (std::min) parenthesized to defeat windows.h's `min` macro.
    const size_t limit = (std::min)(g_snapshotCandidateIdx + kSnapshotChunkSize,
                                    g_snapshotCandidates.size());
    int sent = 0;
    for (; g_snapshotCandidateIdx < limit; ++g_snapshotCandidateIdx) {
        void* obj = g_snapshotCandidates[g_snapshotCandidateIdx];
        // Re-validate liveness: an actor that was live during Phase-1
        // enumeration might have been GC'd in the intervening ticks.
        if (!obj || !R::IsLive(obj)) continue;
        coop::net::PropSpawnPayload p{};
        const std::wstring cls = R::ClassNameOf(obj);
        // Same wire-suppress allowlist as the Init POST observer.
        // Intermediate-variant classes (mushroom7_C) never cross the wire
        // -- host-authoritative.
        if (coop::prop_lifecycle::IsWireSuppressedPropClass(cls)) {
            UE_LOGI("snapshot: skipping intermediate-variant '%ls' actor %p (wire-suppressed)",
                    cls.c_str(), obj);
            continue;
        }
        p.className.len = 0;
        for (size_t j = 0; j < cls.size() && j < 63; ++j) {
            p.className.data[p.className.len++] = static_cast<char>(cls[j]);
        }
        const std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(obj);
        // FName(NAME_None) stringifies to "None" -- unkeyed props are
        // non-syncable (no stable cross-peer identity). Symmetric with
        // prop_lifecycle.cpp Init POST + DestroyActor PRE guards.
        if (keyStr.empty() || keyStr == L"None") continue;
        p.key.len = 0;
        for (size_t j = 0; j < keyStr.size() && j < 31; ++j) {
            p.key.data[p.key.len++] = static_cast<char>(keyStr[j]);
        }
        const auto loc = ue_wrap::engine::GetActorLocation(obj);
        const auto rot = ue_wrap::engine::GetActorRotation(obj);
        p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
        p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
        p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
        p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
        p.scaleX = 1.f; p.scaleY = 1.f; p.scaleZ = 1.f;
        p.physFlags = coop::net::propspawn_flags::kSimulatePhysics;
        if (ue_wrap::prop::IsHeavy(obj))  p.physFlags |= coop::net::propspawn_flags::kIsHeavy;
        if (ue_wrap::prop::IsFrozen(obj)) p.physFlags |= coop::net::propspawn_flags::kFrozen;
        p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
        p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;
        // Use the cached eid from StartEnumerationFor (no per-candidate
        // g_propElementsMutex acquisition; audit fix 2026-05-28).
        {
            const coop::element::ElementId eid =
                g_snapshotCandidateIdx < g_snapshotEids.size()
                    ? g_snapshotEids[g_snapshotCandidateIdx]
                    : coop::element::kInvalidId;
            p.elementId = (eid == coop::element::kInvalidId) ? 0u : eid;
        }
        // Send to ONE slot only. Other peers (already-connected) already
        // have these props from their own connect-edge drain.
        s->SendReliableToSlot(g_currentTargetSlot,
                              coop::net::ReliableKind::PropSpawn,
                              &p, sizeof(p));
        ++sent;
    }
    if (g_snapshotCandidateIdx >= g_snapshotCandidates.size()) {
        UE_LOGI("snapshot: drain complete for slot %d (%zu candidates processed)",
                g_currentTargetSlot, g_snapshotCandidates.size());
        g_snapshotCandidates.clear();
        g_snapshotCandidates.shrink_to_fit(); g_snapshotEids.clear(); g_snapshotEids.shrink_to_fit();
        g_snapshotCandidateIdx = 0;
        g_currentTargetSlot = -1;
        // Dequeue next pending slot if any. This kicks off a fresh
        // enumeration (the world may have changed since the prior drain
        // started -- props spawned/destroyed/moved).
        if (!g_pendingSlots.empty()) {
            const int nextSlot = g_pendingSlots.front();
            g_pendingSlots.erase(g_pendingSlots.begin());
            if (s->IsSlotConnected(nextSlot)) {
                StartEnumerationFor(nextSlot);
            } else {
                UE_LOGI("snapshot: dequeued slot %d already disconnected -- skipping",
                        nextSlot);
            }
        }
    } else {
        UE_LOGI("snapshot: drained chunk for slot %d -- this tick=%d, processed %zu/%zu",
                g_currentTargetSlot, sent, g_snapshotCandidateIdx, g_snapshotCandidates.size());
    }
}

void CancelForSlot(int peerSlot) {
    // Remove `peerSlot` from the pending queue (may have queued + then
    // disconnected before its turn came up).
    g_pendingSlots.erase(
        std::remove(g_pendingSlots.begin(), g_pendingSlots.end(), peerSlot),
        g_pendingSlots.end());
    // If `peerSlot` was the in-progress drain target, abort + dequeue next.
    if (g_currentTargetSlot != peerSlot) return;
    g_snapshotCandidates.clear();
    g_snapshotCandidates.shrink_to_fit(); g_snapshotEids.clear(); g_snapshotEids.shrink_to_fit();
    g_snapshotCandidateIdx = 0;
    g_currentTargetSlot = -1;
    if (!g_pendingSlots.empty()) {
        const int nextSlot = g_pendingSlots.front();
        g_pendingSlots.erase(g_pendingSlots.begin());
        auto* s = g_session_ptr.load(std::memory_order_acquire);
        if (s && s->IsSlotConnected(nextSlot)) {
            StartEnumerationFor(nextSlot);
        }
    }
}

size_t OnDisconnect() {
    size_t pending = 0;
    if (!g_snapshotCandidates.empty()) {
        pending = g_snapshotCandidates.size() - g_snapshotCandidateIdx;
    }
    g_snapshotCandidates.clear();
    g_snapshotCandidates.shrink_to_fit(); g_snapshotEids.clear(); g_snapshotEids.shrink_to_fit();
    g_snapshotCandidateIdx = 0;
    g_currentTargetSlot = -1;
    g_pendingSlots.clear();
    g_pendingSlots.shrink_to_fit();
    return pending;
}

}  // namespace coop::prop_snapshot
