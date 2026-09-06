// coop/prop_snapshot.h -- the connect-time prop snapshot.
//
// When the session reaches Connected the host enumerates every live Aprop_C derivative and
// broadcasts a PropSpawn for each, so a joiner's world converges on the host's. The client's
// OnSpawn de-dupes on FindByKeyString: an existing actor is skipped, a missing one is created,
// and a transform mismatch converges to the host's truth. The work is two-phase, one phase per
// declaration below -- an enumerate on the connected edge, then a bounded drain per net-pump
// tick.

#pragma once

#include <cstddef>
#include <vector>

namespace coop::net { class Session; }

namespace coop::prop_snapshot {

// Cache session pointer (read at Trigger + DrainChunk time). Called once
// at startup from harness.cpp.
void SetSession(coop::net::Session* session);

// PHASE 1. Per-slot snapshot replay; `peerSlot` is a coop::players::Registry slot index
// (1..kMaxPeers-1 for clients on the host). With no drain in progress it copies the element
// Registry's Prop rows -- actor, eid and internal index -- under the registry mutex, skipping
// the dead and the dying, and sets the drain target; with a drain to a DIFFERENT slot in
// progress it queues `peerSlot` for afterwards. The dying test is IsLiveByIndex on the
// GUObjectArray slot rather than IsLive on the pointer, because a mass purge fires no
// K2_DestroyActor and reading a freed pointer faults. So the per-reconnect cost is a bounded
// copy out of a tracked structure, not a GUObjectArray walk, and no ProcessEvent dispatch.
// Host-only sender (no-op + log on a client); the drain is pumped one chunk per NetPumpTick.
void TriggerForSlot(int peerSlot);

// PHASE 2. Drain up to kSnapshotChunkSize=100 candidates per call: read the transform, build
// the payload, call session.SendReliableToSlot for the current target slot. No-op when no
// drain is in progress. Called from NetPumpTick each frame while connected; the reliable
// channel buffers internally, so the cost is spread across frames.
void DrainChunk();

// Broadcast ONE additive PropSpawn for a single runtime-adopted prop WITHOUT opening a snapshot
// bracket. The steady-world re-seed (net_pump) calls this per newly-tracked prop instead of
// re-firing the full bracketed snapshot: a SnapshotBegin/Complete bracket RE-ARMS the client's
// destructive divergence sweep, and a bracket-free add does not. Reuses the EXACT per-prop
// payload logic DrainChunk uses (keyed vs keyless/eid-only pile handling, wire-suppress and
// per-player skips, physics + identity). Host-only; a silent no-op for a non-expressible actor
// (dead / suppressed / per-player / unkeyed-non-pile). MTA's shape: one CEntityAddPacket per
// runtime entity, never a world re-send (Server/.../CStaticFunctionDefinitions.cpp).
void ExpressIncrementalSpawn(void* actor);

// Does the express path above actually BROADCAST right now? It returns immediately on a client,
// so a caller that narrates "broadcasting one PropSpawn each" owes this question first -- one
// printed that sentence over 3,102 adoptions and broadcast nothing. Reads the same session
// pointer the express path does, so the answer cannot disagree with the behaviour.
bool ExpressWouldBroadcast();

// Deliver a kerfur OFF-prop, which ExpressIncrementalSpawn deliberately skips at its kerfur
// guard because a kerfur's only steady-state signal is KerfurConvert. This is the join-window
// deliver-missing owner for a host turn-off whose KerfurConvert never fired: the death-watch
// raced the host's one-shot world-NPC registration, and the off-prop also post-dates the join
// snapshot. Only safe for a re-seed-NEW, i.e. un-converted, off-prop; the client dedups by eid
// through kerfur_prop_adoption::Arm. OWNER BOUNDARY -- JOIN-EDGE ONLY, steady state stays
// KerfurConvert-primary. Host-only; see the .cpp for the window itself.
void ExpressIncrementalKerfurOffProp(void* actor);

// THE host-side late-registration deliver-missing owner: the steady-world re-seed (net_pump)
// hands this every prop it newly adopted into tracking (one no fast channel had delivered yet)
// and this delivers each exactly once -- a generic prop via ExpressIncrementalSpawn, a kerfur
// off-prop via ExpressIncrementalKerfurOffProp. The join-edge backstop that makes the
// per-mutation channels accelerators. Host-only; the client's idempotent apply absorbs any
// overlap.
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
