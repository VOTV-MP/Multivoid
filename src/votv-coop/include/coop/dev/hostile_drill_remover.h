// coop/dev/hostile_drill_remover.h -- the remover_far, remover_hidden and remover_flood drill arms, proving the host
// judges a client's RemoverRequest by what it can see and bounds what it costs. Defensive testing of OUR OWN mod, per
// coop/dev/hostile_drill.h; nothing here ships enabled.
//
//   remover_far     a joined client sends one request for a real host prop whose origin lies 2000 uu from where the
//                   host holds its body; the HOST must answer OutOfSight and answer nothing else.
//   remover_hidden  a joined client picks a host prop its own camera cannot see (its trace meets level geometry first)
//                   and sends one request from its real camera at that prop; the HOST must answer OutOfSight.
//   remover_flood   a joined client sends, in one tick, 121 requests each naming an unused host-range id; the host
//                   must serve them at the lane's pace and queue no more than its cap, judged by its own per-sender
//                   counts: once all 121 are accounted for, whether the runs reached the cap, followed the pace from
//                   the first run, and never held more than the cap unserved and unrefused.
// The host judges by its own per-sender answer and run counts against a baseline taken when it arms, never by a
// symbol of a log. A prop is only removed by a lane that fails the judge.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill::remover {

// The sub-arm; None when the active drill arm is not ours.
enum class Arm { None, RemoverFar, RemoverHidden, RemoverFlood };

// The arm's name, for the verdict markers.
const char* ArmName(Arm a);

// Client (sender), game thread: once this peer's own join is over, send the arm's requests once.
void TickClient(coop::net::Session* s, Arm a);

// Host (judge), game thread: arm on the first world-ready slot, then judge and print the PASS / FAIL /
// INCONCLUSIVE marker.
void TickHostJudge(coop::net::Session* s, Arm a);

// Back to the un-armed state (disconnect / re-arm).
void OnDisconnect();

}  // namespace coop::dev::hostile_drill::remover
