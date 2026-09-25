// coop/dev/toggle_drill.h -- the symmetric toggle lanes' drill: does a device's toggle cross, both
// ways. No walk: each peer fires the device's own verb through reflection, as a player's use or the
// wall button dispatches it, and waits on what the other did:
//   HOST   -- once hosting, picks the device of the drilled kind with the lowest key in its lane and
//             reads its state.
//   CLIENT -- once its join is over, picks the same device, toggles it and reads its copy at once
//             (the host's answer can land before its next tick).
//   HOST   -- whenever its copy shows a client's toggle, toggles it back, and waits again.
//   CLIENT -- once its copy shows the host's toggle, says DONE.
// A toggle waits for its device to take one (a garage mid-swing ignores its runTrigger), not for a
// clock; a step whose change never arrives ends at a bound and says so. The kinds (toggle_drill=):
// garage (its runTrigger), tap (a faucet's or a sink's action 5), locker (its action 10). Lines are
// tagged [TOGGLE-DRILL]; run on both peers of a pair; the client's DONE line ends it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::toggle_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single check when off. One device read a tick once picked.
void Tick(coop::net::Session* session);

// The device and the phase belong to one world.
void OnDisconnect();

}  // namespace coop::dev::toggle_drill
