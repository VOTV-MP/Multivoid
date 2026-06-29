// coop/pile_reconcile.h -- the join-bracket keyless-pile position-bind reconcile.
//
// EXTRACTED from coop/remote_prop_spawn.cpp 2026-06-23 (RULE 2026-05-25 modular
// file-size: remote_prop_spawn.cpp had reached the 1500 LOC hard cap; this is the
// conceptually-distinct "reconcile the host's keyless chipPile expression against
// the client's save-loaded native pile set" subsystem). Behavior preserved
// byte-for-byte from the in-line implementation.
//
// WHY this subsystem exists: with the save-transfer join the client's world is
// LOADED FROM THE HOST'S OWN SAVE -- its chipPiles are the same piles, settled at
// the same positions. So the host's keyless-eid pile expression is reconciled
// against the client's OWN local pile (instead of sweep-destroying all ~870 and
// fresh-spawning mirrors). Two paths, both built off ONE lazily-built bracket-
// scoped GUObjectArray index (NOT one walk per pile):
//   - TryDestroyTwin: a host pile PROXY (AStaticMeshActor) spawned at `eid`; the
//     client's co-located save-loaded NATIVE twin is the visible DUP -> destroy it
//     so the proxy is the sole mirror. The match position is a PARAMETER: v86
//     Path 1c passes the pile's SAVE-TIME position (the frozen value both peers
//     loaded from the same save) so a pile the host MOVED in the join-load window
//     still reconciles (native@old vs the save-time key@old) -- pre-1c the match
//     was the proxy's CURRENT pose, which goes blind on a moved pile = the two-
//     channel join-window DUP (votv-pile-dup-join-window-two-channel finding).
//   - FindAndConsumeAdoptCandidate: the eid-only fresh-spawn fallback ADOPTS a
//     live same-class/chipType native within 30cm onto the host eid (the caller
//     registers the mirror + reconciles physics).
//
// Game-thread ONLY (the event_feed drain). No mutex -- same contract as the claim
// set it reads. The `claimed` set (remote_prop_spawn's g_claimedActors) is passed
// in read-only so an already-bound native is skipped.

#pragma once

#include "coop/element/element.h"  // coop::element::ElementId (ArmPendingSaveTimeTwin)
#include "coop/net/protocol.h"
#include "ue_wrap/types.h"  // ue_wrap::FVector

#include <string>
#include <unordered_set>

namespace coop::pile_reconcile {

// Drop the bracket-scoped index (bracket open/close). Mirrors the claim set's
// lifecycle: called at BeginClaimTracking + every sweep/teardown.
void Reset();

// A host pile PROXY for `payload.elementId` just spawned. Destroy the client's
// save-loaded NATIVE twin matched by `matchPos` (the SAVE-TIME key under Path 1c;
// the proxy's render pose pre-1c). chipType-gated, ambiguous-skip (>1 within 1cm
// -> keep all). Lazily builds the index on first call. No-op if no twin.
//
// `isSaveTimeKey` (v86 Path 1c): true when matchPos is the pile's frozen SAVE-TIME
// position (payload.hasMatchPos). On a MISS (matchCount==0) with isSaveTimeKey, the
// twin is RECORDED for a retry at the post-quiescence sweep -- because the world-ready
// snapshot burst runs BEFORE the client's async native-pile load-tail has drained, so
// a moved pile's save-loaded native@old may not exist/be-indexed yet at this call (it
// loads in the tail; the divergence sweep sees it ~10s later). See SweepReconcileSaveTimeTwins.
void TryDestroyTwin(const coop::net::PropSpawnPayload& payload,
                    const ue_wrap::FVector& matchPos,
                    bool isSaveTimeKey,
                    const std::unordered_set<void*>& claimed);

// Post-quiescence retry of the save-time twin-destroys that MISSED at world-ready (their
// native@save-time-key had not async-loaded yet). Call from RunDivergenceSweep_ -- which
// runs ONLY after load-tail quiescence (TickClientReconcile gates on kSweepQuiesceScans
// stable scans), so the late natives are now present (the orphan census sees them). For
// each pending (eid -> save-time key), a FRESH GUObjectArray walk finds the now-loaded
// native within 1cm + same chipType and DESTROYS it (the proxy@new from the same save-time
// key is the sole mirror). SAFETY: a >50%-of-live-natives abort-valve (mirrors the sweep's
// own valve) refuses a removal that big = a racing/incomplete bracket, not real divergence.
// Returns the count removed. Game-thread only (the sweep). Clears the pending set.
int SweepReconcileSaveTimeTwins();

// docs/piles/09 (4th mirror-identity instance): arm a pending save-time twin DIRECTLY from
// the wire (not via TryDestroyTwin's bracket-scoped index). The client calls this from
// remote_prop::OnConvert when a kToPile LAND carries hasMatchPos=1 (the host self-seeded the
// eid at an in-window grab + stamped the pre-grab pos). The bracket-independent
// SweepReconcileSaveTimeTwins then retires the stale native@old at quiescence -- the same
// L1 cure, but keyed at the GRAB edge so it survives the in-window identity (eid) change.
// Idempotent per eid (a re-grab/land overwrites with the latest key). Game-thread only.
void ArmPendingSaveTimeTwin(coop::element::ElementId eid, const ue_wrap::FVector& savePos, uint8_t chipType);

// eid-only adopt: find + CONSUME (remove from the index) a live same-`classW`,
// same-chipType native within 30cm of payload.loc. Returns the pile actor (the
// caller registers it as the host-eid mirror + reconciles physics), or nullptr.
// On a hit, *outD2 = the matched squared distance and *outBindSeq = the shared
// per-bracket bind-log counter (throttles the caller's log identically to the
// pre-extraction in-line counter).
void* FindAndConsumeAdoptCandidate(const coop::net::PropSpawnPayload& payload,
                                   const std::wstring& classW,
                                   const std::unordered_set<void*>& claimed,
                                   float* outD2, int* outBindSeq);

// L1 orphan census (logged once per join at the divergence sweep). FRESH GC-robust
// GUObjectArray walk (NOT the build-time index -- a mass-purge at the sweep churns
// the array, staling stored internal indices). No-op if the index was never built
// this bracket. See the in-line note this replaced.
void LogCensus();

// b3 (v90, PropSnapPos): a join-window position correction for a save-authoritative chipPile the host MOVED
// while the joiner's reliable channel wasn't ready (the move's PropConvert was dropped, and chipPiles carry
// no position in the connect-snapshot -- both peers load them from the identical save, so the bind cements
// the native at the STALE save pos). The client ARMS the correction on receipt (event_dispatch_entity) and
// APPLIES it at the quiescence sweep: resolve the bound native by eid (the bind has registered it + the load
// tail is settled) -> SetActorLocation/Rotation to the host's authoritative pos -> log drift. Identity is
// preserved (the native stays a native; this is a position delta on the existing save-authoritative identity,
// NOT a re-spawn), and idempotent (an eid already corrected by a delivered convert snaps to the same pos ->
// drift 0). research/findings/coop-b3-window-moved-pile-position-DESIGN-2026-06-26.md.
void ArmPendingPosCorrection(coop::element::ElementId eid,
                             const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot);

// Drain the armed b3 corrections (applied ones erased). Called from RunDivergenceSweep_ (quiescence) and,
// for a late arrival after the sweep already fired, immediately from the receive handler. Game-thread only.
void ApplyPendingPosCorrections();

// True iff there is armed-but-unconsumed reconcile work (a pending save-time twin OR a pending b3 position
// correction). The steady-state reconcile trigger (coop::sync::OnReconcileTick) polls this so it only does a
// GUObjectArray walk when there is actually something to reconcile -- NOT every tick (the perf rule). The D1
// structural fix: a save-pile grabbed/moved AFTER the join one-shot arms a twin that this predicate then
// surfaces to the steady reconcile. Game-thread only.
bool HasPendingWork();

}  // namespace coop::pile_reconcile
