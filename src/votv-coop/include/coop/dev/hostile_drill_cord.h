// coop/dev/hostile_drill_cord.h -- the cord_* drill arms, proving the host refuses what a client's cord link claim may
// not do, applies or answers what it may, and counts each outcome under its own reason. Defensive testing of OUR OWN
// mod, per coop/dev/hostile_drill.h; nothing here ships enabled.
//
// A joined client forges CordLinkState messages through the session's own send, bypassing its own cord lane, and the
// HOST judges by accounting: every message is exactly one claim that ends in one outcome or one dispatcher drop, so the
// sum of the lane's counter deltas since its baseline equals the messages sent and each counter's delta matches the
// arm's table. A claim is answered whatever its outcome; the client counts the answers it sees (each is dropped by its
// pending rule, for the forged claim registered nothing) and tells the host in the lobby chat, so the host's verdict
// covers both ends.
//   cord_malformed  cord_bad_op  cord_reach  cord_flood  cord_dead_sender
// The client records the coffee makers it already holds and says so in the lobby chat; the host then spawns the one
// corded prop the arm needs before the client's puppet (a near one, or one 2000 uu out) and says it is ready in the
// lobby chat. The family acts on slot 1 only; another slot idles.

#pragma once

#include <string>

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill::cord {

// The sub-arm; None when the active drill arm is not ours. DeadSender: the client kills its own player once it sees the
// maker, the host says ready only while that puppet reads dead, and the claim is sent inside that death.
enum class Arm { None, Malformed, BadOp, Reach, Flood, DeadSender };

// The arm a config value names; None for a value that is not one of ours.
Arm ArmOf(const std::string& value);

// The arm's name, for the verdict markers.
const char* ArmName(Arm a);

// Host (judge), game thread: spawn the corded prop the arm needs, say ready, then judge by accounting and print PASS /
// FAIL / INCONCLUSIVE.
void TickHostJudge(coop::net::Session* s, Arm a);

// Client (sender), game thread: record the start set and say so, wait for the host's ready line, find the host's prop,
// send the arm's forged messages, then count the answers.
void TickClient(coop::net::Session* s, Arm a);

// Back to the un-armed state (disconnect / re-arm).
void OnDisconnect();

}  // namespace coop::dev::hostile_drill::cord
