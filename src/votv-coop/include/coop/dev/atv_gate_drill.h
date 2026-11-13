// coop/dev/atv_gate_drill.h -- [dev] the ATV drill: each act a player does at an ATV, done the player's way, and every outcome
// decided by an event of the ATV host path (coop/interactables/atv_drill_events.h), never by a clock.
// One drill thread per peer, armed by a value of the `atv_gate_drill` row on EVERY peer: `green` is the whole run and each
// other value is a red, read only by the module whose mechanism it disables. Three peers: the host, the client in slot 1 (it
// leaves and rejoins, mp.py --rejoin) and an observer in slot 2 (mp.py --observer). The peers order their steps through
// harness/autotest/peer_signal and move only with the bot director. The host names the world's ATV nearest it by route.
// A verdict is said the moment it is decided, by the host, on `rig: VERDICT atv PASS|FAIL|INVALID`; a FAIL or an INVALID
// ends every peer's sequence at once, and a time bound can only say INVALID. Each step says `atv-drill: step <id> PASS|FAIL|
// INVALID: <what>`.
#pragma once

#include <windows.h>

namespace coop::net { class Session; }

namespace coop::dev::atv_gate_drill {

// The drill thread for the harness dispatch (CreateThread): host, client or observer by the role and the slot.
DWORD WINAPI Thread(LPVOID arg);

// The rejoined client holds its world-ready announce from the session's start (a latched read unless the drill row is on and
// this is the second life). Every load tick.
void Install(coop::net::Session* session);

}  // namespace coop::dev::atv_gate_drill
