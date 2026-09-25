// coop/world/event_fire_sync.h -- HOST-AUTHORITATIVE scheduled-event replay channel.
// VOTV's scripted story events (list_events DataTable, 69 rows) fire through saveSlot::settime
// -> eventer.runEvent, a BP->BP EX_LocalVirtualFunction chain below both hook seams that only the
// script-body gate sees, and level-placed event flips ride no other lane. Three mechanisms carry
// the channel, all bytecode-verified: the HOST watches runEvent through the gate and broadcasts a
// call whose caller is settime, the one call that fires a scheduled row; the CLIENT holds
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

// Cache the session; register the host's watch on runEvent (once per process); and, once the
// daynightCycle_C class has loaded, register the client's hold on the cycle's own tick (once per
// process): right before each tick, allEvents.Num is held at 0. The cycle's settime walks that list
// on every clock change the host's samples make, so without the hold due rows would fire natively
// on the client -- and the first sample after a world load, which the clock lane writes at the same
// tick, could make many due at once. Restored on disconnect; the local world resumes scheduling.
// Called every pump tick by the install fanout. Game thread.
//
// Three classes of fire the watch leaves alone, all deliberate. A trigger-volume fire (bedEvent,
// a scare armed by TBoxActivator) executes per-peer natively when THAT peer overlaps, which is
// per-viewer by the game's own design. The game's internal runSpecialEvent picks have no settime
// caller, so their outputs ride the prop lane. And a dev fire reaches runEvent from our own
// ProcessEvent, not from settime -- the native ui_eventRun likewise -- which is why HostFire
// broadcasts at dispatch, and why the scheduler may still re-fire that row at its scheduled time
// with the client's replayed-set deduping its side.
void Install(coop::net::Session* session);

// The ONE native-fire primitive + the dev broadcast seam. Posts the reflected
// runEvent/runSpecialEvent to the game thread (resolution happens inside the task -- a SOLO
// host's dev menu has no session); when host+connected also broadcasts EventFire{kind,name} (a dev
// fire has no settime caller, so the watch does not see it). specialName crosses ONLY into the
// local native call (RandomPrank = L"ariralPrank"); the wire carries the name alone. Callable from
// any thread (the menu's render thread included). Returns false only when refused (connected as a
// client -- host is authoritative).
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

}  // namespace coop::event_fire_sync
