// coop/dev/hostile_drill_hand.h -- drill arms proving a hand-item message cannot make a peer keep or build
// an actor of a class that is not a hand item. A joined client describes its own hand as holding a chosen
// class, then sends one chat line behind it on the same lane, and the HOST judges at that line.
// bad_hand_class names the engine's base actor class, not a prop: the host judges the class stored for the
// slot, then the slot's display mirror, read after a mirror pass that ran with the slot's puppet up (the
// pass skips a slot without one). bad_hand_family names the per-player inventory container's class, a prop
// the coop layer never puts on the wire, spelt in another letter case (class names compare without case):
// the host judges only whether it was stored. Defensive testing of OUR OWN mod, per coop/dev/hostile_drill.h;
// nothing here ships enabled.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill::hand {

// The sub-arms; None when the active drill arm is not one of ours.
enum class Arm { None, BadHandClass, BadHandFamily };

// The arm's name, for the verdict markers.
const char* ArmName(Arm a);

// Client (sender), game thread: once this peer's own join is over, send the forged hand state and its
// sentinel line, once.
void TickClient(coop::net::Session* s, Arm a);

// Host (judge), game thread: arm on the first world-ready slot, then judge at the sentinel and print the
// PASS / FAIL / INCONCLUSIVE marker.
void TickHostJudge(coop::net::Session* s, Arm a);

// Back to the un-armed state (disconnect / re-arm).
void OnDisconnect();

}  // namespace coop::dev::hostile_drill::hand
