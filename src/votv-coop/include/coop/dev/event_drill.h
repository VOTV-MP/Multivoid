// coop/dev/event_drill.h -- the drill of the scheduled-event lanes, which see the game's own verbs:
// runEvent for a fire and its cue, setEvent for an event's life. The host fires the meteor shower
// (the starRain row, whose body spawns the shower's emitter) twice, once through each route:
//   - as the client's transport connects, before its world is up: the dev fire (HostFire, the F1
//     menu's seam, through our own ProcessEvent). The cue's send is dropped for the loading slot, so
//     the shower reaches the joiner through its world-ready join snapshot, as for a mid-shower join;
//   - once its world is up: the scheduler itself. The clock is set to the second day at 00:18, a
//     minute past the row's 00:17 and the only row due by then, so the cycle's next settime calls
//     runEvent('starRain'), which the event lane sees coming from settime and the cue lane as it
//     returns.
// Then dev fires the policy decides differently: solar (replayed, a power-down with no lane of its own),
// arirGraff_0 (a special, replayed) and enasus (its props ride the prop lane: not replayed). The
// client's DONE says it replayed both showers and the two fires. The host's save must be before the
// second day's 00:17 or it says FAIL, so the drill runs on a New Game (mp.py smoke --host-fresh), the
// rig's save being past that hour. Tagged [EVENT-DRILL]; both peers (event_drill=1).

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::event_drill {

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::event_drill
