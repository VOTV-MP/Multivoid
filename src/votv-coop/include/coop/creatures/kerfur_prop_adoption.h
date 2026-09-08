// coop/kerfur_prop_adoption.h -- DEFERRED class+pose adoption for PROP-form kerfurs at join.
//
// A kerfur PROP (prop_kerfurOmega_C) is host-owned with a blueprint key minted at random per peer,
// so a client cannot match its host twin by key. At join the host expresses each one as a PropSpawn
// and the client's inline fuzzy match in remote_prop_spawn::OnSpawn -- class plus pose within 30 cm
// -- runs ONCE on receipt: if the client's save-loaded twin has not async-loaded yet, it MISSES and
// a DUPLICATE is spawned beside it. That orphan is untracked, so ClaimConversionGhosts over-claims
// and destroys it, and the conversion poll cannot see it.
//
// So a fuzzy miss ARMS a pending adoption here instead, and a 5 Hz poll binds the local twin by
// class and nearest pose once it materialises, gated on HasLoadTailQuiesced, fresh-spawning only as
// a last resort. The bound twin is then one host-range MIRROR: claimed, sweep-safe, excluded from
// ClaimConversionGhosts and poll-visible. The prop-form analogue of coop/npc_adoption. CLIENT-only,
// GAME-THREAD-only, no mutex; the client-mint gate keeps the twin out of the Registry's actor-to-eid map, so
// after adoption it is purely a mirror.

#pragma once

#include "coop/net/protocol.h"  // PropSpawnPayload

namespace coop::kerfur_prop_adoption {

// CLIENT: a host kerfur-prop PropSpawn arrived but the inline fuzzy match found no local twin (it may
// still be async-loading). Defer: arm a pending adoption resolved by class+nearest-pose on the poll.
// Idempotent on the payload's elementId. Called from remote_prop_spawn::OnSpawn. Game thread.
void Arm(const coop::net::PropSpawnPayload& payload);

// CLIENT: 5 Hz poll (resolve pending) + nothing else. Called from the client tick (beside
// npc_adoption::Tick). Cheap no-op when no entries are pending. Game thread.
void Tick();

// The connect snapshot is fully delivered. (Parity with npc_adoption -- reserved for a future
// pending-converged barrier; currently informational.) Game thread.
void OnSnapshotComplete();

// A fresh connect replay is about to deliver this world's PropSpawns -- drop stale pending. Game thread.
void OnClientWorldReady();

// Net disconnect / session teardown -- drop all pending. Game thread.
void OnSessionEnd();

}  // namespace coop::kerfur_prop_adoption
