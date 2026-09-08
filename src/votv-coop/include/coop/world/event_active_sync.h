// coop/world/event_active_sync.h -- the native ACTIVE-EVENT registry mirror, so a peer can join
// during an event.
//
// The game keeps its own in-flight event registry on mainGamemode_C: `activeEvents`, an int
// refcount, and `activeEvents_senders`, a TArray<UObject*> of the live event actors. The single
// writer is lib_C::setEvent, which each of about ninety-five event classes calls on itself as it
// begins and ends. That call is a lib_C CDO dispatch from blueprint internals, invisible to a
// hook, but the poll reads the RESULT, so no hook is needed.
//   - HOST, ~1 Hz: poll activeEvents_senders, diff membership by object identity, edge-log
//     `event_active: BEGIN/END class=<sender class> n=<refcount>` with per-event elapsed time.
//   - HOST, at a joiner's world-ready edge: one ReliableKind::EventSnapshot per in-flight entry,
//     carrying {className, mapped list_events row, elapsedSec}.
//   - CLIENT: receive EventSnapshot and hand mapped replay-safe rows to event_fire_sync's
//     active-override replay, where an in-flight row bypasses the InClientPassEvents dedupe --
//     the joiner's blob already carries it as history. An unmapped class logs LOUD and skips.

#pragma once

namespace coop::net {
class Session;
struct EventSnapshotPayload;
}  // namespace coop::net

namespace coop::event_active_sync {

// Cache the session. Resolution (gamemode class + the two property offsets) is lazy in Tick.
void Install(coop::net::Session* session);

// Per net-pump tick, game thread, throttled to ~1 Hz internally. HOST and connected only:
// the senders membership diff, which is both the BEGIN/END edge log and the join snapshot's
// source of truth. A no-op on the client, which has no local event actors -- the registry
// reaches a joiner per lane and per snapshot, and the refcount itself is never mirrored.
void Tick();

// HOST, game thread, at a joiner's ClientWorldReady edge (subsystems::ConnectReplayForSlot):
// send one EventSnapshot per in-flight registry entry to this slot.
void SendJoinSnapshotForSlot(int slot);

// CLIENT, game thread (event_feed drain): one in-flight event entry from the host. Logs it;
// mapped rows go to event_fire_sync::ReplayInFlightRow (policy + active-override live there).
void OnReliable(const coop::net::EventSnapshotPayload& payload);

// Teardown: drop the tracked membership + baseline + cached gamemode, clear the session.
void OnDisconnect();

}  // namespace coop::event_active_sync
