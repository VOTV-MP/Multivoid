// The pile SPAWN-TIME native-bind mechanism.
//
// A joiner's world comes from the host's save, so the pile a host expression names is one the
// client already owns, at the same position. This resolves the expression against the client's
// OWN save-loaded native chipPile set -- through a lazily-built bracket-scoped GUObjectArray
// index rather than one walk per pile -- and binds that actor as the host eid's mirror, so the
// client keeps the game's own pile with its collision, its hover prompt and its look-at trace
// (docs/piles.md).
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
#include <vector>

namespace coop::pile_spawn_bind {

// Drop the bracket-scoped index (bracket open/close). Mirrors the claim set's lifecycle:
// called at BeginClaimTracking + every sweep/teardown. Resets ONLY the spawn-time index --
// the DEFERRED reconcile queues live in quiescence_drain and survive the bracket (they
// drain at quiescence / steady-state), cleared only at session teardown.
void Reset();

// Bind the client's own save-loaded native for the trash expression `payload`, a pile or a clump
// at rest (the index holds both forms, and a row binds a native of its own form only): match it at
// `matchPos` (bit-exact within 1 cm, same chipType, ambiguous cluster skipped so the wrong one is
// never bound), consume it from the index, claim it for the membership sweep,
// converge its transform when it genuinely diverged, and register it as
// the mirror at `payload.elementId`, marked save-native.
// Returns the bound native, or nullptr when nothing matched. Lazily builds the index on the first
// call.
//
// `isSaveTimeKey`: true when matchPos is the pile's frozen SAVE-TIME position
// (payload.hasMatchPos). On a MISS with isSaveTimeKey the expression is recorded on the order
// owner (quiescence_drain::ArmPendingSaveTimeTwin) for a retry at the post-quiescence sweep --
// the world-ready snapshot burst runs BEFORE the client's async native-pile load tail has
// drained, so the native at that position may not exist yet at this call and appears in the tail
// about ten seconds later.
void* BindOwnSavePile(const coop::net::PropSpawnPayload& payload,
                      const std::wstring& classW,
                      const ue_wrap::FVector& matchPos,
                      bool isSaveTimeKey,
                      int senderSlot,
                      const std::unordered_set<void*>& claimed);

// The session is over: every clump of this client's own that a bind parked gets its tick and its
// physics back, and its drive cleared. Called BEFORE remote_prop::ForceRelease, which destroys a
// driven clump as a mirror nobody owns. Game thread.
void OnDisconnect();

// Adopt `native` as the mirror of `eid`: claim it for the membership sweep (a no-op outside a
// bracket), register it as the mirror and mark it save-native,
// which is the flag the grab route, the morph hand-off, the sweep exemption and the retire all
// read. The TRANSFORM is the caller's: the spawn path converges to the host pose, and the
// order owner leaves the host's own position correction to snap a late bind. `why` names the
// path in the log. Game thread.
void AdoptOwnNative(void* native, uint32_t eid, int senderSlot, const char* why);

// L1 orphan census (logged once per join, called by the quiescence_drain sequence on the join
// sweep). FRESH GC-robust GUObjectArray walk (NOT the build-time index -- a mass-purge at the
// sweep churns the array, staling stored internal indices). Reports the leftover native chipPiles
// no arriving expression bound, banded by distance to the nearest bound native. No-op
// if the index was never built this bracket. Reads this module's own build-time count + dark-probe
// gate -- it reports on THIS module's bind outcome, so it lives here. Game-thread only.
void LogCensus();

// Every live native trash actor (a chip pile or a garbage clump, CDOs out) from a FRESH walk of the
// object array, appended to `out` after it is cleared. The one walk the orphan census and the join
// sweep's universe share; it reads no bracket state, so it answers whether or not the bind index was
// built. COLD: once per join. Game thread only.
void CollectLiveTrashNatives(std::vector<void*>& out);

}  // namespace coop::pile_spawn_bind
