// coop/dev/light_drill.h -- the light lanes' drill: a client's switch press moves the host's group,
// and never its own copy alone. No walk: each peer fires the switch's own use() through reflection,
// as a player's press dispatches it, and waits on what the other did:
//   HOST   -- once hosting, picks the switch with the lowest key in the switch lane whose group the
//             group lane names and whose breaker is on, and reads the group's state.
//   CLIENT -- once its join is over, picks the same switch and, once its instrument is live, presses
//             it and reads its own copy straight after: the switch flipped and the group left.
//   HOST   -- whenever its group shows a client's press, presses the switch back, and waits again.
//   CLIENT -- once the group lane has applied the host's change and then the host's press to its
//             copy (counted at the group's own runTrigger, so two in one tick are both seen), DONE.
// Each peer logs every change of that group's state. Lines are tagged [LIGHT-DRILL]. Run on both
// peers of a pair (light_drill=1); the client's DONE line ends it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::light_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off. One group read a tick once picked.
void Tick(coop::net::Session* session);

// The switch, its group and the phase belong to one world.
void OnDisconnect();

}  // namespace coop::dev::light_drill
