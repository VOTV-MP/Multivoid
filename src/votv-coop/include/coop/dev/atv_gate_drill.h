// coop/dev/atv_gate_drill.h -- [dev] the ATV gate probe's drill: each act a player does at an ATV, done the player's way,
// so the probe (coop/dev/atv_gate_probe) says which of the ATV's own functions that act reaches on each peer.
//
// One drill thread per peer, armed by atv_gate_drill=1 on BOTH peers together with atv_gate_probe=1 (the probe is the
// instrument; without it the drill says INVALID). The peers order their steps through harness/autotest/peer_signal
// (VOTVCOOP_CAN_DIR) and move only with the bot director. The host picks the world's ATV nearest to it by route and names
// its key; the client holds a copy under it. The client's steps, the host observing:
//   can toolbox fit take sit lights brake turbo honk leave
// then the host walks to the ATV and does host_can host_toolbox host_sit host_lights host_brake host_turbo host_honk,
// the client observing. Each peer prints, per step, one line of what its probe saw while the step ran:
//   [ATVG-DRILL] MEASURE <step> role=<host|client> acted=<0|1> sg=<fn:n,...|-> pe=<fn:n,...|-> fuel=<a>><b> health=<a>><b>
//                modules=<a>><b> seated=<0|1>
// It ends on rig: VERDICT atvprobe PASS (every step ran) or INVALID with the reason: the drill measures, it has no failing
// arm. The red arm is atv_gate_probe=0, where the drill says INVALID.
#pragma once

#include <windows.h>

namespace coop::dev::atv_gate_drill {

// The drill thread for the harness dispatch (CreateThread): host or client by the role.
DWORD WINAPI Thread(LPVOID arg);

}  // namespace coop::dev::atv_gate_drill
