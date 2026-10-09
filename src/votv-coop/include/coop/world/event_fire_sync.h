// coop/world/event_fire_sync.h -- HOST-AUTHORITATIVE scheduled-event replay channel.
// VOTV's scripted story events (list_events DataTable, 69 rows) fire through saveSlot::settime
// -> eventer.runEvent, a BP->BP EX_LocalVirtualFunction chain below both hook seams that only the
// script-body gate sees, and level-placed event flips ride no other lane. Three mechanisms carry
// the channel, all bytecode-verified: the HOST watches BOTH eventer verbs through the gate and
// broadcasts each call a body commits -- a scheduled row's runEvent, a dev or menu fire's, and
// the runSpecialEvent the prank roll picks inside summonArirPrank, one emit per occurrence; the
// CLIENT holds saveSlot.allEvents.Num at 0, because settime's walk iterates that array rather
// than the DataTable, and mainGamemode's boot ubergraph rebuilds allEvents FROM that DataTable
// unconditionally at every world load, so the zeroed count self-heals and can never poison a
// save; and the client REPLAYS the same native verb for allowlisted rows only, which is our own
// per-row policy -- it and its reasoning are in docs/events-and-weather.md.
// One owner: this module owns the whole scheduled-event authority axis -- suppression,
// observation, the native-fire primitive and replay. The F1 dev menu (coop/dev/event_trigger)
// dispatches THROUGH HostFire, so dev depends on coop/world and never the reverse.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct EventFirePayload;
}  // namespace coop::net

namespace coop::event_fire_sync {

// Which native verb fired / to replay. ON THE WIRE (EventFirePayload.dispatch) -- append-only.
enum class FireKind : uint8_t {
    RunEvent = 0,      // trigger_eventer.runEvent(name, special)
    SpecialEvent = 1,  // trigger_eventer.runSpecialEvent(name)
};

// Game-thread pump: cache the session, register both host watches, drain pending replays and
// hold the client's allEvents.Num at zero before the cycle tick. This prevents settime from
// firing local scheduled events on a received clock sample. Disconnect restores the list.
// Trigger-volume scares remain per viewer; host menu, scheduler and prank fires emit through
// the same watches. A repeated scheduled row still meets the client's replayed-set dedupe.
void Install(coop::net::Session* session);

// The ONE native-fire primitive. Posts the reflected runEvent/runSpecialEvent to the game thread
// (resolution happens inside the task -- a SOLO host's dev menu has no session); the watches
// emit every host fire to the clients, this call included, so a dev fire broadcasts exactly once
// through the same seam. specialName crosses ONLY into the local native call (RandomPrank =
// L"ariralPrank"); the wire carries the name each verb took -- for a prank roll, the CHOSEN case
// emitted at its own runSpecialEvent. Callable from any thread (the menu's render thread
// included). Returns false when refused as a running client, including while joining.
bool HostFire(FireKind kind, const std::wstring& eventName, const std::wstring& specialName);

// CLIENT receiver (event_dispatch_world, reliable drain, game thread): replay per policy, or
// queue until the eventer resolves (join window). Host receiving its own kind = dropped upstream.
void OnReliable(const coop::net::EventFirePayload& payload);

// CLIENT, game thread (event_active_sync's EventSnapshot receiver): the host says this
// list_events row is IN FLIGHT right now. Same per-row policy as OnReliable (lane-owned /
// host-local / unknown rows skip), but a replay-safe row replays with the ACTIVE-OVERRIDE: the
// InClientPassEvents dedupe is bypassed, since it exists for COMPLETED history and the joiner's
// blob already carries this row. The session replayed-set still applies (a world-change re-sync
// resends the snapshot; the row must not replay twice). Always FireKind::RunEvent -- registry
// senders are scheduled/story rows, and runSpecialEvent never registers.
void ReplayInFlightRow(const std::string& rowName);

// CLIENT, game thread, at its world-ready announce (net_pump): replay the fires queued in the join
// window, in arrival order. The gamemode's boot has set its eventer long before the announce's
// gates (a world up, a registry seeded, a quiet load tail) all hold.
void OnClientWorldReady();

// Teardown: restore the client's allEvents.Num (SP scheduler resumes), clear the pending queue +
// replayed-set, drop the session pointer. Game thread.
void OnDisconnect();

// [dev] How many fires this client has replayed natively, for the event drill. Any thread.
unsigned ReplayCount();

// Dev (the event drill): on a client, dispatch runEvent('solar') as a reflected call holding no
// admission, and say whether the gate refused it. Game thread; false off a client or with the
// watch not live (the call is then not made).
bool DevProbeClientRefusal();

}  // namespace coop::event_fire_sync
