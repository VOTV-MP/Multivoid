// coop/dev/appliance_drill.h -- the appliance lane's drill: does a faucet's tap cross, both ways.
// No walk: each peer fires the tap's own toggle (actionOptionIndex, the faucet's action 5) through
// reflection, as a player's use of the aimed tap dispatches it, and waits on what the other did:
//   HOST   -- once hosting, picks the faucet with the lowest key in the appliance lane and reads its
//             state.
//   CLIENT -- once its join is over, picks the same faucet and toggles it.
//   HOST   -- whenever its copy shows a client's toggle, toggles it back, and waits again.
//   CLIENT -- once its copy shows the host's toggle, says DONE.
// Each peer logs every change of that faucet's state; a step whose change never arrives ends at a
// bound and says so. Lines are tagged [APPL-DRILL]. Run on both peers of a pair (appliance_drill=1);
// the client's DONE line ends it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::appliance_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off. One faucet read a tick once picked.
void Tick(coop::net::Session* session);

// The faucet and the phase belong to one world.
void OnDisconnect();

}  // namespace coop::dev::appliance_drill
