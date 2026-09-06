// coop/world/event_fire_sync.h -- HOST-AUTHORITATIVE scheduled-event replay channel.
// VOTV's scripted story events (list_events DataTable, 69 rows) fire through saveSlot::settime
// -> eventer.runEvent, a BP->BP EX_LocalVirtualFunction chain invisible to every hook we own,
// and level-placed event flips ride no other lane. Three mechanisms carry the channel, the
// first two bytecode-verified: the HOST polls saveSlot.passEvents for GROWTH, because settime
// appends each fired row there while runEvent never touches the array; the CLIENT holds
// saveSlot.allEvents.Num at 0, because settime's walk iterates that array rather than the
// DataTable, and mainGamemode's boot ubergraph rebuilds allEvents FROM that DataTable
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

// Cache the session. Resolution (gamemode/saveSlot/eventer offsets) is lazy in Tick. Game thread.
void Install(coop::net::Session* session);

// Per net-pump tick, game thread, ~1 Hz internally throttled:
//   HOST + connected: passEvents growth poll -> broadcast new fires.
//   CLIENT: assert allEvents.Num == 0 (scheduler suppression) + drain pending replays.
//
// The client assert is what closes the sleep-accelerate hole: during an accelerate the client
// clock free-runs at TimeScale=1 and its own settime walk RUNS, so day-boundary rows would
// otherwise fire natively there. Restored on disconnect; the local world resumes scheduling.
//
// Three classes of fire this poll cannot see, all deliberate. A trigger-volume fire (bedEvent,
// a scare armed by TBoxActivator) executes per-peer natively when THAT peer overlaps, which
// is per-viewer by the game's own design. The game's internal runSpecialEvent picks append
// no passEvents row, so their outputs ride the prop lane. And a dev fire appends nothing to
// the HOST's passEvents -- the native ui_eventRun behaves the same -- which is why HostFire
// broadcasts at dispatch, and why the scheduler may still re-fire that row at its scheduled
// time with the client's replayed-set deduping its side.
void Tick();

// The ONE native-fire primitive + the dev broadcast seam. Posts the reflected
// runEvent/runSpecialEvent to the game thread (resolution happens inside the task -- a SOLO
// host's dev menu never runs the connected-gated Tick); when host+connected also broadcasts
// EventFire{kind,name} (a dev fire never reaches passEvents, so the poll cannot see it).
// specialName crosses ONLY into the local native call (RandomPrank = L"ariralPrank"); the wire
// carries the name alone. Callable from any thread (the menu's render thread included).
// Returns false only when refused (connected as a client -- host is authoritative).
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

// Teardown: restore the client's allEvents.Num (SP scheduler resumes), clear poll baseline +
// pending queue + replayed-set, drop the session pointer. Game thread.
void OnDisconnect();

}  // namespace coop::event_fire_sync
