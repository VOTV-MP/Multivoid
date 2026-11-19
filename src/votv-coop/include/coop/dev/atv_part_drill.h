// coop/dev/atv_part_drill.h -- [dev] a client's work on an ATV's wheel, judged on both peers.
//
// The part lane (coop/interactables/atv_part_intent) cancels a client's own body and asks the host. The host
// spawns an ATV beside the players and names it in the lobby chat. Arm `atv_part_drill=wheel`, five legs: the
// client unbolts a wheel with an empty hand (refused), unbolts two wheels with a wrench and fits a held wheel
// (each run; both peers show the same tyre mask and wheel props), then sends a second fit on the hold the first
// spent (refused, nothing given back).
//
// Every leg is decided by an event, never a clock: the host says what its hand lane holds of the client's hand
// and its counters, each peer says its copy's mask and props when they change. A time bound can only say
// INVALID. A FAIL or INVALID of either peer is told to the other, so both logs carry `rig: VERDICT atv-part`; the
// host's PASS rests on its own counters (ran 3, refused 2, refunded 0). Reds, each failing at its leg: row
// `atv_part_red` (cancel_off, tool_judge_off) and the host row `hand_spend_drill=hand_spend_no_latch`. Run with
// join_at_host=1.

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
