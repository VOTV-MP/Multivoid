// The pile SPAWN-TIME native-bind mechanism.
//
// At a host pile PROXY spawn, reconcile that proxy against the client's OWN
// save-loaded native chipPile set, through a lazily-built bracket-scoped
// GUObjectArray index rather than one walk per pile. The joiner's world came from
// the host's save, so its chipPiles ARE the host's piles at the same positions --
// which is why a keyless-eid expression can bind to a local pile instead of
// sweep-destroying ~870 of them and spawning mirrors (docs/piles.md).
//
// This is the SPAWN-time half only, driven by remote_prop_spawn during a PropSpawn;
// the DRAIN-time half -- the deferred queues this arms and the ordered sweep that
// drains them -- belongs to coop/element/quiescence_drain.h, the order owner.
//
// Game-thread ONLY (the event_feed drain), no mutex: the same contract as the
// claim set it reads. `claimed` (remote_prop_spawn's g_claimedActors) is read-only.

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

// A host pile PROXY for `payload.elementId` just spawned. Destroy the client's
// save-loaded NATIVE twin matched by `matchPos`. chipType-gated, ambiguous-skip
// (>1 within 1cm -> keep all). Lazily builds the index on first call. No-op if no
// twin.
//
// `isSaveTimeKey`: true when matchPos is the pile's frozen SAVE-TIME position
// (payload.hasMatchPos). On a MISS (matchCount==0) with isSaveTimeKey, the twin is
// recorded on the order owner (quiescence_drain::ArmPendingSaveTimeTwin) for a
// retry at the post-quiescence sweep -- the world-ready snapshot burst runs BEFORE
// the client's async native-pile load-tail has drained, so a moved pile's
// save-loaded native at the old position may not exist yet at this call; it loads
// in the tail about ten seconds later.
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
