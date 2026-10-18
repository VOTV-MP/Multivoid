// coop/dev/hostile_drill_act.h -- the act_* drill arms, proving the host judges a client's PropActRequest and
// PropActCommit by what it can see and bounds what they cost. Defensive testing of OUR OWN mod, per
// coop/dev/hostile_drill.h; nothing here ships enabled.
//
//   act_far          a joined client asks to take a host prop 3000 uu from its body: Reach, and the prop stands.
//   act_gone         it asks for a prop the host destroyed: Gone.
//   act_wrongfamily  it forges six acts the game never performs on four props: WrongFamily, each by its own term.
//   act_bad_commit   it commits an id it never asked for, is granted a real take, aborts it, and aborts it again:
//                    two bad commits tallied and the prop stands.
//   act_notyet       it asks with its world up and its announce held, then lets the announce go: the host waits and
//                    answers once, after the announce.
//   act_flood        it asks 121 times in one tick: the queue never holds more than its cap, and each ask is answered.
//   act_flood_early  the same flood before the announce: the asks past the cap are counted, not answered.
// The host judges by its own counters against a baseline taken when it arms. A client that reads a verdict (act_notyet,
// act_flood) reads its own count of replies, after the host says it judged.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill::act {

// The sub-arm; None when the active drill arm is not ours.
enum class Arm { None, ActFar, ActGone, ActWrongFamily, ActBadCommit, ActNotYet, ActFlood, ActFloodEarly };

// The arm's name, for the verdict markers.
const char* ArmName(Arm a);

// Session start, game thread: a client on a pre-announce arm holds its own announce (once per session).
void Install(coop::net::Session* s, Arm a);

// Client (sender), game thread: send the arm's requests once this peer's state allows, and read its own verdict
// where the arm has one.
void TickClient(coop::net::Session* s, Arm a);

// Host (judge), game thread: arm on the arm's slot, then judge and print the PASS / FAIL / INCONCLUSIVE marker.
void TickHostJudge(coop::net::Session* s, Arm a);

// Back to the un-armed state (disconnect / re-arm).
void OnDisconnect();

}  // namespace coop::dev::hostile_drill::act
