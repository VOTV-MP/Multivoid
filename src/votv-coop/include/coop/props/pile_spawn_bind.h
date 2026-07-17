// coop/props/pile_spawn_bind.h -- the pile SPAWN-TIME native-bind mechanism.
//
// ONE concept: at a host pile PROXY spawn, reconcile that proxy against the client's
// OWN save-loaded native chipPile set, using a lazily-built bracket-scoped GUObjectArray
// index. This is the SPAWN-time half of the keyless-pile reconcile -- it is driven by
// remote_prop_spawn during a PropSpawn, NOT by the quiescence sequence. (The DRAIN-time
// half -- the deferred queues this can arm + the ordered sweep that drains them -- lives
// in coop/element/quiescence_drain.h, the join-window ORDER owner. The split was the
// 2026-06-30 anti-smear refactor: pile_reconcile.cpp held BOTH this spawn mechanism AND
// the order owner's queues, plus a generic kerfur destroy queue -- three concepts in one
// mis-named file. [[feedback-one-owner-order-axis]])
//
// EXTRACTED from coop/props/pile_reconcile.cpp 2026-06-30; that file's group-A symbols
// (the bracket index + TryDestroyTwin + FindAndConsumeAdoptCandidate + the [PILE-DELTA]
// probe) moved here byte-for-byte; only the miss-arm now calls quiescence_drain::Arm*
// across modules (the spawn mechanism CAPTURES into the order owner's queue).
//
// WHY this works: with the save-transfer join the client's world is LOADED FROM THE
// HOST'S OWN SAVE -- its chipPiles are the same piles, settled at the same positions.
// So the host's keyless-eid pile expression is reconciled against the client's OWN local
// pile (instead of sweep-destroying all ~870 and fresh-spawning mirrors). Two paths, both
// built off ONE lazily-built bracket-scoped index (NOT one walk per pile):
//   - TryDestroyTwin: a host pile PROXY (AStaticMeshActor) spawned at `eid`; the client's
//     co-located save-loaded NATIVE twin is the visible DUP -> destroy it so the proxy is
//     the sole mirror. The match position is a PARAMETER: v86 Path 1c passes the pile's
//     SAVE-TIME position (the frozen value both peers loaded) so a pile the host MOVED in
//     the join-load window still reconciles. On a save-time-key MISS, the twin is recorded
//     on the order owner (quiescence_drain::ArmPendingSaveTimeTwin) for the post-quiescence
//     retry (the native@old loads ~10s later, in the async tail).
//   - FindAndConsumeAdoptCandidate: the eid-only fresh-spawn fallback ADOPTS a live same-
//     class/chipType native within 30cm onto the host eid (the caller registers the mirror).
//
// Game-thread ONLY (the event_feed drain). No mutex -- same contract as the claim set it
// reads. The `claimed` set (remote_prop_spawn's g_claimedActors) is passed in read-only.

#pragma once

#include "coop/net/protocol.h"
#include "ue_wrap/core/types.h"  // ue_wrap::FVector

#include <string>
#include <unordered_set>

namespace coop::pile_spawn_bind {

// Drop the bracket-scoped index (bracket open/close). Mirrors the claim set's lifecycle:
// called at BeginClaimTracking + every sweep/teardown. Resets ONLY the spawn-time index --
// the DEFERRED reconcile queues live in quiescence_drain and survive the bracket (they
// drain at quiescence / steady-state), cleared only at session teardown.
void Reset();

// A host pile PROXY for `payload.elementId` just spawned. Destroy the client's save-loaded
// NATIVE twin matched by `matchPos` (the SAVE-TIME key under Path 1c; the proxy's render
// pose pre-1c). chipType-gated, ambiguous-skip (>1 within 1cm -> keep all). Lazily builds
// the index on first call. No-op if no twin.
//
// `isSaveTimeKey` (v86 Path 1c): true when matchPos is the pile's frozen SAVE-TIME position
// (payload.hasMatchPos). On a MISS (matchCount==0) with isSaveTimeKey, the twin is recorded
// on the order owner (quiescence_drain::ArmPendingSaveTimeTwin) for a retry at the post-
// quiescence sweep -- because the world-ready snapshot burst runs BEFORE the client's async
// native-pile load-tail has drained, so a moved pile's save-loaded native@old may not exist
// yet at this call (it loads in the tail ~10s later).
void TryDestroyTwin(const coop::net::PropSpawnPayload& payload,
                    const ue_wrap::FVector& matchPos,
                    bool isSaveTimeKey,
                    const std::unordered_set<void*>& claimed);

// eid-only adopt: find + CONSUME (remove from the index) a live same-`classW`, same-chipType
// native within 30cm of payload.loc. Returns the pile actor (the caller registers it as the
// host-eid mirror + reconciles physics), or nullptr. On a hit, *outD2 = the matched squared
// distance and *outBindSeq = the shared per-bracket bind-log counter.
void* FindAndConsumeAdoptCandidate(const coop::net::PropSpawnPayload& payload,
                                   const std::wstring& classW,
                                   const std::unordered_set<void*>& claimed,
                                   float* outD2, int* outBindSeq);

// L1 orphan census (logged once per join, called by the quiescence_drain sequence on the join
// sweep). FRESH GC-robust GUObjectArray walk (NOT the build-time index -- a mass-purge at the
// sweep churns the array, staling stored internal indices). Reports the leftover native chipPiles
// no arriving proxy claimed within 1cm, banded by distance to the nearest live pile proxy. No-op
// if the index was never built this bracket. Reads this module's own build-time count + dark-probe
// gate -- it reports on THIS module's bind outcome, so it lives here. Game-thread only.
void LogCensus();

}  // namespace coop::pile_spawn_bind
