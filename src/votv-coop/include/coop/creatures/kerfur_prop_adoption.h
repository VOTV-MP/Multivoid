// coop/kerfur_prop_adoption.h -- DEFERRED class+pose adoption for PROP-form kerfurs at join.
//
// THE prop-form join fix (K-6, 2026-06-16). A kerfur PROP (prop_kerfurOmega_C) is a host-owned entity
// whose BP key is RANDOM per peer (and late-minted), so a client cannot match its host twin by key.
// At join the host's connect snapshot expresses each kerfur prop via PropSpawn; the client's INLINE
// fuzzy match (remote_prop_spawn::OnSpawn Gap-I-1, 30 cm class+pose) runs ONCE on receipt -- and if the
// client's own save-loaded twin hasn't async-loaded yet, the fuzzy MISSES and OnSpawn fresh-spawns a
// DUPLICATE beside the still-loading twin. The orphaned untracked local then (a) gets over-claimed +
// destroyed by kerfur_convert::ClaimConversionGhosts and (b) is invisible to the conversion poll (only
// MirrorManager<Prop> is scanned) -> a client turn-on of it never reaches the host. (Full RCA:
// research/findings/votv-kerfur-prop-join-adoption-RCA-AND-DESIGN-2026-06-16.md.)
//
// This module is the PROP-form analogue of coop/npc_adoption.{h,cpp} (the WORKING template the NPC
// form already uses): instead of fresh-spawning on a fuzzy miss, OnSpawn ARMS a pending adoption here;
// a 5 Hz poll binds the local twin by class + nearest-pose once it materializes (gated on
// HasLoadTailQuiesced), fresh-spawning only as a last resort. The bound twin becomes a single
// host-range MIRROR -> claimed (sweep-safe), excluded from ClaimConversionGhosts (IsMirror), and
// poll-visible (turn-on works). One logical kerfur, one prop mirror, no duplicate.
//
// CLIENT-only, GAME-THREAD-only (no mutex; g_pending + the latches are GT-only, exactly like
// npc_adoption). Built on the SHIPPED K-5 (the client-mint gate keeps the local twin out of
// g_actorToPropElementId, so it is purely a mirror after adoption).

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
