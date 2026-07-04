// coop/world/event_active_sync.h -- the native ACTIVE-EVENT registry mirror (join-during-event).
//
// The game keeps its own in-flight event registry on mainGamemode_C: `activeEvents` (int
// refcount) + `activeEvents_senders` (TArray<UObject*> of the live event actors), single writer
// lib_C::setEvent (RAII-style self-registration by ~95 event classes; bytecode-verified,
// research/findings/votv-active-events-registry-RE-2026-07-04.md). setEvent itself is a lib_C CDO
// call from BP internals (EX-dispatch, hook-invisible) -- but the poll reads the RESULT, so no
// hook is needed (the passEvents-poll shape from event_fire_sync, proven at v95).
//
// This module (docs/COOP_EVENT_JOIN.md):
//   - HOST, ~1 Hz: polls activeEvents_senders, diffs membership by object identity, edge-logs
//     `event_active: BEGIN/END class=<sender class> n=<refcount>` with per-event elapsed time
//     (Phase 0, seam PROVEN 2026-07-04 on forced obelisk + alarm chain + piramid e2e);
//   - HOST, at a joiner's world-ready edge: sends one ReliableKind::EventSnapshot per in-flight
//     entry {className, mapped list_events row, elapsedSec} (Phase 1, v98);
//   - CLIENT: receives EventSnapshot and hands mapped replay-safe rows to event_fire_sync's
//     active-override replay (an in-flight row bypasses the InClientPassEvents dedupe -- the
//     joiner's blob already carries the row as "history"). Unmapped classes log LOUD + skip;
//     the class->row map fills per-event (Phase 2 completes the ~95 census).
//
// The per-lane late-join contract table lives in docs/COOP_EVENT_JOIN.md section 3.4.

#pragma once

namespace coop::net {
class Session;
struct EventSnapshotPayload;
}  // namespace coop::net

namespace coop::event_active_sync {

// Cache the session. Resolution (gamemode class + the two property offsets) is lazy in Tick.
void Install(coop::net::Session* session);

// Per net-pump tick, game thread, ~1 Hz internally throttled. HOST + connected only:
// senders membership diff -> BEGIN/END edge log + the join-snapshot source of truth. No-op on
// the client (a joiner has no local event actors; the registry is delivered per-lane /
// per-snapshot, never counter-mirrored -- accepted asymmetry, COOP_EVENT_JOIN.md section 3.3).
void Tick();

// HOST, game thread, at a joiner's ClientWorldReady edge (subsystems::ConnectReplayForSlot):
// send one EventSnapshot per in-flight registry entry to this slot (Phase 1, v98).
void SendJoinSnapshotForSlot(int slot);

// CLIENT, game thread (event_feed drain): one in-flight event entry from the host. Logs it;
// mapped rows go to event_fire_sync::ReplayInFlightRow (policy + active-override live there).
void OnReliable(const coop::net::EventSnapshotPayload& payload);

// Teardown: drop the tracked membership + baseline + cached gamemode, clear the session.
void OnDisconnect();

}  // namespace coop::event_active_sync
