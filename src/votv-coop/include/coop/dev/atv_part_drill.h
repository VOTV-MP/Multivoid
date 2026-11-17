// coop/dev/atv_part_drill.h -- [dev] a client's work on an ATV's wheel, judged on both peers.
//
// The part lane (coop/interactables/atv_part_intent) cancels a client's own body and asks the host.
// The host spawns an ATV beside the players and names it in the lobby chat. Arm (`atv_part_drill`):
// wheel -- the client runs the wrench's last step on wheel 0 of its copy, and both peers must lose
// that wheel and gain one wheel prop; the client then takes a wheel into its hand and runs putTire,
// and both must show the wheel on again, the hand empty. Each peer says its copy's tyre mask and its
// count of wheel props whenever they change; the client judges every step against both, the host
// judges its own counters when the client says it is done. Every verdict is one line,
// `[ATV-PART-DRILL]` with the peer's tag and `PASS` or `FAIL`. Run with join_at_host=1, so the client
// stands within the host's reach of the ATV.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::dev::atv_part_drill {

bool IsEnabled();

// The drill's pass on either peer. A single read when off. Game thread.
void Tick(coop::net::Session* session);

// Back to the first step. Net disconnect.
void OnDisconnect();

}  // namespace coop::dev::atv_part_drill
