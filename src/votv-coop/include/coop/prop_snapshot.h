// coop/prop_snapshot.h -- Phase 5S0 save snapshot bootstrap.
//
// USER DIRECTIVE 2026-05-24 ("client just reconnects to the host's game
// and all objects and states are force synced"): when the session reaches
// Connected, host enumerates every live Aprop_C derivative in GUObjectArray
// and broadcasts a PropSpawn for each. Client OnSpawn de-dupes on
// FindByKeyString -- existing actors are skipped; missing actors are
// created; transform mismatches converge to host's truth.
//
// Two-phase design (audit I-2 2026-05-24; enumeration source updated
// 2026-05-28 per H2-redux):
//   Phase 1 (TriggerForSlot on host-connected edge): snapshot-copies
//   prop_lifecycle's maintained known-keyed-props set into the local
//   candidate vector. The set is seeded ONCE at Install via a single
//   GUObjectArray walk and kept current via Init POST insert +
//   K2_DestroyActor PRE evict -- per-reconnect cost is O(set-size)
//   pointer copy (~2000) instead of O(GUObjectArray) walk (~150k).
//   No ProcessEvent dispatch.
//   Phase 2 (per NetPumpTick): drain kSnapshotChunkSize=100 candidates,
//   reading their transforms and calling SendReliableToSlot. The
//   reliable channel buffers internally; ~330-500 ms total wall-clock,
//   spread across frames.

#pragma once

#include <cstddef>

namespace coop::net { class Session; }

namespace coop::prop_snapshot {

// Cache session pointer (read at Trigger + DrainChunk time). Called once
// at startup from harness.cpp.
void SetSession(coop::net::Session* session);

// Per-slot snapshot replay. `peerSlot` is a coop::players::Registry
// slot index (1..kMaxPeers-1 for clients on host). The function:
//   - if no drain is currently in progress, enumerates live keyed-
//     interactable Aprop_C derivatives into the internal candidate
//     vector and sets the drain target to `peerSlot`;
//   - if a drain to a DIFFERENT slot is already in progress, queues
//     `peerSlot` for after the current drain completes.
// Host-only sender (no-op + log if called on client). The drain is
// pumped one chunk per NetPumpTick frame via DrainChunk().
void TriggerForSlot(int peerSlot);

// Phase 2: drain up to chunkSize candidates per call (read transform,
// build payload, call session.SendReliableToSlot for the current target
// slot). No-op when no drain is in progress. Called from NetPumpTick
// each frame while connected.
void DrainChunk();

// Abort any pending or in-progress drain for `peerSlot`. Called from
// the harness's per-slot disconnect edge so a peer drop mid-drain
// doesn't waste ~1700 SendReliableToSlot calls into a dead connection
// (Session silently no-ops these but still iterates candidates). If
// the in-progress drain target is `peerSlot`, dequeues the next
// pending slot if any.
void CancelForSlot(int peerSlot);

// Reset internal state on AGGREGATE disconnect (all peers gone). Returns
// count of candidates that were enumerated but not yet drained.
size_t OnDisconnect();

}  // namespace coop::prop_snapshot
