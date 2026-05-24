// coop/prop_snapshot.h -- Phase 5S0 save snapshot bootstrap.
//
// USER DIRECTIVE 2026-05-24 ("client just reconnects to the host's game
// and all objects and states are force synced"): when the session reaches
// Connected, host enumerates every live Aprop_C derivative in GUObjectArray
// and broadcasts a PropSpawn for each. Client OnSpawn de-dupes on
// FindByKeyString -- existing actors are skipped; missing actors are
// created; transform mismatches converge to host's truth.
//
// Two-phase design (audit I-2 2026-05-24):
//   Phase 1 (Trigger on host-connected edge): single GUObjectArray walk
//   collects ~2000 Aprop_C pointers. No ProcessEvent dispatch.
//   Phase 2 (per NetPumpTick): drain kSnapshotChunkSize=100 candidates,
//   reading their transforms and enqueueing PropSpawn via the
//   prop_lifecycle retry queue. ~330-500 ms total, spread across frames.

#pragma once

#include <cstddef>

namespace coop::net { class Session; }

namespace coop::prop_snapshot {

// Cache session pointer (read at Trigger + DrainChunk time). Called once
// at startup from harness.cpp.
void SetSession(coop::net::Session* session);

// Phase 1: host-only enumeration of live Aprop_C candidates into the
// internal vector. Reset the per-chunk drain index. No-op + log if not
// host or not connected. Called from the NetPumpTick connect-edge.
void Trigger();

// Phase 2: drain up to chunkSize candidates per call (read transform,
// build payload, enqueue via prop_lifecycle::EnqueuePropSpawnForRetry).
// No-op when enumeration not in progress. Called from NetPumpTick each
// frame while connected.
void DrainChunk();

// Reset internal state on disconnect. Returns count of candidates that
// were enumerated but not yet drained (for the disconnect log line).
size_t OnDisconnect();

}  // namespace coop::prop_snapshot
