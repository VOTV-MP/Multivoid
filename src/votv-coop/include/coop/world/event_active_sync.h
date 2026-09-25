// coop/world/event_active_sync.h -- the native ACTIVE-EVENT registry mirror, so a peer can join
// during an event.
//
// The game keeps its own in-flight event registry on mainGamemode_C: `activeEvents`, an int
// refcount, and `activeEvents_senders`, a TArray<UObject*> of the live event actors. The single
// writer is lib_C::setEvent, which each of about ninety-five event classes calls on itself as it
// begins and ends: a lib_C CDO dispatch from blueprint internals, which the script-body gate sees.
//   - HOST: a watch on setEvent logs each edge as the game makes it,
//     `event_active: BEGIN/END class=<sender class> n=<refcount>`, with the event's elapsed time.
//   - HOST, at a joiner's world-ready edge: the game's own registry read there, one
//     ReliableKind::EventSnapshot per in-flight entry, carrying {className, mapped list_events row,
//     elapsedSec}; elapsed is 0 for an event that began before this host watched.
//   - CLIENT: receive EventSnapshot and hand mapped replay-safe rows to event_fire_sync's
//     active-override replay, where an in-flight row bypasses the InClientPassEvents dedupe --
//     the joiner's blob already carries it as history. An unmapped class logs LOUD and skips.

#pragma once

namespace coop::net {
class Session;
struct EventSnapshotPayload;
}  // namespace coop::net

namespace coop::event_active_sync {

// Cache the session and register the host's watch on setEvent (once per process). Called every pump
// tick by the install fanout, which is also the retry until the gate resolves the watch's name.
// Game thread.
void Install(coop::net::Session* session);

// HOST, game thread, at a joiner's ClientWorldReady edge (subsystems::ConnectReplayForSlot):
// send one EventSnapshot per in-flight registry entry to this slot.
void SendJoinSnapshotForSlot(int slot);

// CLIENT, game thread (event_feed drain): one in-flight event entry from the host. Logs it;
// mapped rows go to event_fire_sync::ReplayInFlightRow (policy + active-override live there).
void OnReliable(const coop::net::EventSnapshotPayload& payload);

// Teardown: drop the begin times, clear the session.
void OnDisconnect();

}  // namespace coop::event_active_sync
