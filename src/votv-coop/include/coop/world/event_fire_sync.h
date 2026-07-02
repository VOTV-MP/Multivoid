// coop/world/event_fire_sync.h -- HOST-AUTHORITATIVE scheduled-event replay channel (v95).
//
// THE GAP (docs/COOP_SYNC_MAP "DESIGNED, NOT BUILT" 2026-07-03 -> built here): VOTV's scripted
// story events (list_events DataTable, 69 rows) fire via saveSlot::settime -> eventer.runEvent --
// a BP->BP EX_LocalVirtualFunction chain INVISIBLE to every hook we own (COOP_DISPATCH_VISIBILITY).
// Level-placed event flips (the campfire / treehouse builds / server breaks / forceObjects
// signals) ride NO existing lane, so a client never sees them fire. This module is the missing
// channel, host-authoritative per the catalog-B verdict (_events_catalog_B_scheduler.md section 5A):
//
//   - HOST OBSERVATION: settime appends each fired row to saveSlot.passEvents (Array_Add,
//     bytecode-verified saveSlot.json settime; runEvent itself never touches the array). The host
//     Tick polls passEvents GROWTH (~1 Hz, two int reads steady-state) and broadcasts
//     EventFire{dispatch,rowName} for each new entry. Poll-only is impossible for dev fires (a
//     direct runEvent never appends), so the dev seam below broadcasts at dispatch.
//   - CLIENT SUPPRESSION: settime's event walk iterates saveSlot.allEvents (NOT the DataTable --
//     bytecode-verified). The client Tick keeps allEvents.Num == 0 (a one-int write; a legal
//     TArray state; mainGamemode's boot ubergraph rebuilds allEvents from the DataTable
//     UNCONDITIONALLY every world load, so this can never poison a save). This closes the
//     sleep-accelerate hole: during the v71 accelerate phase the client clock free-runs
//     (TimeScale=1), its settime walk RUNS, and day-boundary rows (treehouse_N at 00:00) would
//     fire natively on the client -- the "dormant scheduler" claim (event_cue_sync) was only true
//     outside sleep. Restored on disconnect (the local SP world resumes scheduling).
//   - CLIENT REPLAY, PER-ROW POLICY: the receiver replays the SAME native verb reflected
//     (runEvent(name, None) / runSpecialEvent(name)) -- but ONLY for rows on the REPLAY allowlist
//     (the dupe matrix, votv-event-system-RE-2026-06-13.md section 10/10.4 ground truth). Default
//     is NO-replay: rows whose outputs already ride a lane (prop/npc/atv/sleep/wisp/event_cue/
//     device) would DOUBLE-deliver (the client-local dup disease), creature/save-actor spawns
//     would create client-local actors, and prank specials are host-local RNG. Replayed rows are
//     the level-flip / save-story / cosmetic-sound ones no lane carries. ariralPrank special is
//     NEVER forwarded (the client would re-roll a DIFFERENT random prank).
//   - DEDUPE: a replayed row is skipped if the client's own passEvents already contains it (the
//     v56 save transfer carried the fire) or it was already replayed this session.
//
// One owner (anti-smear): this module owns the whole scheduled-event authority axis --
// suppression + observation + the native-fire primitive + replay. The F1 dev menu
// (coop/dev/event_trigger) dispatches THROUGH HostFire so dev fires broadcast exactly like
// scheduler fires (dev depends on coop/world, never the reverse).
//
// Known boundaries (documented, deliberate):
//   - Trigger-volume fires (bedEvent, scares armed by TBoxActivator) execute per-peer natively
//     when THAT peer overlaps -- per-viewer by SP design (RE 2026-06-13 section 10.4), not a gap.
//   - The game's own internal runSpecialEvent picks (summonArirPrank) are invisible (no
//     passEvents append) -- prank outputs ride the prop lane, as before this module.
//   - A dev fire does not append the HOST's passEvents (native ui_eventRun behaves the same), so
//     the scheduler can re-fire that row at its scheduled time on the host; the client's
//     replayed-set dedupes its side.

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

// Teardown: restore the client's allEvents.Num (SP scheduler resumes), clear poll baseline +
// pending queue + replayed-set, drop the session pointer. Game thread.
void OnDisconnect();

}  // namespace coop::event_fire_sync
