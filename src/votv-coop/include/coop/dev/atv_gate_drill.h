// coop/dev/atv_gate_drill.h -- [dev] the ATV gate probe's drill: each act a player does at an ATV, done the player's way,
// so the probe (coop/dev/atv_gate_probe) says which of the ATV's own functions that act reaches on each peer.
// One drill thread per peer, armed by atv_gate_drill=1 on BOTH peers with atv_gate_probe=1 (no probe, INVALID). The peers
// order their steps through harness/autotest/peer_signal and move only with the bot director. The host names the world's
// ATV nearest it by route. Client steps, the host observing: can overlap toolbox fit take sit lights brake turbo honk
// idleburn_client leave; then the host walks to the ATV: host_can host_toolbox honkreplay_press honkreplay_release host_sit
// host_lights host_brake host_turbo host_honk idleburn_host host_leave host_fitfly honkreplay_fly_press/_release. can,
// overlap and host_can start from a drained tank; idleburn needs the ATV to move 500 cm or its step is not issued.
//   [ATVG-DRILL] MEASURE <step> role=<host|client> actor=<0|1> acted=<0|1> issued=<0|1> sg=<fn:n,...|-> pe=<fn:n,...|->
//                fuel=<a>><b> health=<a>><b> modules=<a>><b> seated=<0|1> alt=<a>><b> fly=<a>><b> hasFly=<n> [fuelUp_by=...]
//                [driven_by=<peer>][ why=<reason>]; also toolboxcalls, overlap detail and honkreplay_* detail lines.
// An observer waits, bounded, for the act to reach its copy. An act that could not be issued is measured with its why and
// the run goes on. It ends on rig: VERDICT atvprobe PASS (every step issued) or INVALID listing the steps not issued, said
// by the host; there is no failing arm, and atv_gate_probe=0 makes the drill say INVALID.
#pragma once

#include <windows.h>

namespace coop::dev::atv_gate_drill {

// The drill thread for the harness dispatch (CreateThread): host or client by the role.
DWORD WINAPI Thread(LPVOID arg);

}  // namespace coop::dev::atv_gate_drill
