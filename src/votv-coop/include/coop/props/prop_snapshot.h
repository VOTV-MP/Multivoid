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
#include <vector>

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

// R1 (2026-06-17, MTA CEntityAddPacket): broadcast ONE additive PropSpawn for a
// single runtime-adopted prop WITHOUT opening a snapshot bracket. The steady-
// world re-seed (net_pump) calls this per newly-tracked prop instead of re-firing
// the full bracketed snapshot -- a SnapshotBegin/Complete bracket RE-ARMS the
// client's destructive divergence sweep (the join-churn), a bracket-free add does
// not. Reuses the EXACT per-prop payload logic DrainChunk uses (keyed vs
// keyless/eid-only pile handling, wire-suppress/per-player skips, v54
// physics+identity). Host-only (host-authoritative spawns); silent no-op for a
// non-expressible actor (dead / suppressed / per-player / unkeyed-non-pile).
// Mirrors MTA: one CEntityAddPacket per runtime entity, never a world re-send
// (Server/.../CStaticFunctionDefinitions.cpp:8349).
void ExpressIncrementalSpawn(void* actor);

// Deliver a kerfur OFF-prop that the generic ExpressIncrementalSpawn deliberately skips (:568) -- the
// join-window deliver-missing owner for a host turn-off whose KerfurConvert never fired (the death-watch
// raced the host's one-shot world-NPC registration; the off-prop also post-dates the join snapshot).
// Only safe for a re-seed-NEW (== un-converted) off-prop; the client dedups by eid via
// kerfur_prop_adoption::Arm. OWNER BOUNDARY -- JOIN-EDGE ONLY (steady-state stays KerfurConvert-primary);
// see the .cpp + docs/COOP_MIRROR_IDENTITY_WINDOW_RACE.md. Host-only.
void ExpressIncrementalKerfurOffProp(void* actor);

// THE host-side late-registration deliver-missing owner: the steady-world re-seed (net_pump) hands this
// every prop it newly adopted into tracking (one no fast channel had delivered yet) and this delivers
// each exactly once -- a generic prop via ExpressIncrementalSpawn, a kerfur off-prop via
// ExpressIncrementalKerfurOffProp. The join-edge backstop that makes the per-mutation channels
// accelerators. Host-only; the client's idempotent apply absorbs any overlap.
void DeliverLateRegisteredProps(const std::vector<void*>& lateProps);

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
