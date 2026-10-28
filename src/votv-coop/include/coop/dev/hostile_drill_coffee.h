// coop/dev/hostile_drill_coffee.h -- the coffee_* drill arms, proving the host refuses what a client's coffee press or
// coffee-maker record may not do and counts each outcome under its own reason. Defensive testing of OUR OWN mod, per
// coop/dev/hostile_drill.h; nothing here ships enabled.
//
// A joined client forges CoffeeActionIntent messages through the session's own send, bypassing its coffee lane, and
// the HOST judges by accounting: every message is exactly one claim that ends in one outcome or one dispatcher drop,
// so the sum of the lane's counter deltas since its baseline equals the messages sent and each counter's delta matches
// the arm's table. A press has no answer packet, so the host alone judges.
// The six arms, and what each forges, are the table in hostile_drill_coffee.cpp.
// The client records the coffee makers (or crowbars) it already holds and says so in the lobby chat; the host then
// spawns the one prop the arm needs before the client's puppet (a near one, or one 2000 uu out) and says it is ready in
// the lobby chat. The family acts on slot 1 only; another slot idles.

#pragma once

#include <string>

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill::coffee {

enum class Arm { None, Malformed, WrongClass, Reach, Flood, ForgedRecord, DeadSender };

// The arm a config value names; None for a value that is not one of ours.
Arm ArmOf(const std::string& value);

// The arm's name, for the verdict markers.
const char* ArmName(Arm a);

// Host (judge), game thread: spawn the prop the arm needs, say ready, then judge by accounting and print PASS /
// FAIL / INCONCLUSIVE.
void TickHostJudge(coop::net::Session* s, Arm a);

// Client (sender), game thread: record the start set and say so, wait for the host's ready line, find the host's prop,
// send the arm's forged messages.
void TickClient(coop::net::Session* s, Arm a);

// Back to the un-armed state (disconnect / re-arm).
void OnDisconnect();

}  // namespace coop::dev::hostile_drill::coffee
