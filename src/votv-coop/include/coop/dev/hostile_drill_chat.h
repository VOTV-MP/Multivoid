// coop/dev/hostile_drill_chat.h -- the chat arms of the receive-boundary drill; the HOST judges each by its own lobby
// record. Defensive testing of OUR OWN mod, per coop/dev/hostile_drill.h; nothing here ships enabled.
//   chat_ill_formed -- a joined client sends an ill-formed UTF-8 chat line, then a well-formed one at the drills' chat
//                      spacing; the host must refuse the first and commit the second.
//   chat_flood      -- the host must bound how fast a client speaks as Source does. A joined client sends ten lines in
//                      one tick: the host commits the first and drops the rest for good (the interval). Once its first
//                      line is back, the client waits out the debt and sends one more, which must be committed (the
//                      recovery); once that is back and the bucket has refilled, four lines 0.9 s apart, of which the
//                      host must commit three (the bucket).

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill::chat {

// The sub-arm; None when the active drill arm is not one of ours.
enum class Arm { None, ChatIllFormed, ChatFlood };

// The arm's name, for the verdict markers.
const char* ArmName(Arm a);

// Client (sender), game thread: once this peer's own join is over, the arm's lines, each at its own moment.
void TickClient(coop::net::Session* s, Arm a);

// Host (judge), game thread: arm on the first world-ready slot, then judge and print the PASS / FAIL /
// INCONCLUSIVE marker.
void TickHostJudge(coop::net::Session* s, Arm a);

// Back to the un-armed state (disconnect / re-arm).
void OnDisconnect();

}  // namespace coop::dev::hostile_drill::chat
