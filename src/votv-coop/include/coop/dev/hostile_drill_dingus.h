// coop/dev/hostile_drill_dingus.h -- the dingus_report drill arm, proving the host's judge of a client's found report refuses
// what it may not accept and counts each refusal under its own reason. Defensive testing of OUR OWN mod, per
// coop/dev/hostile_drill.h; nothing here ships enabled.
//
// A joined client sends thirteen DingusReport messages in one tick through the session's own send, and the HOST judges by
// accounting: every message ends in exactly one dispatcher drop or one judge outcome, so the counter deltas since the
// baseline sum to the messages sent and each matches the arm's table, and the found set is as it was. The table is in
// hostile_drill_dingus.cpp. The arm names the lane's own symbols (the kind 281 and the judge's drill view), as a new kind must:
// its red is a throwaway build of the lane in which the judge's budget and key checks accept, never landed.
// The host spawns a plain prop, a dingus beside the client's puppet and a dingus 2000 cm out, says the keys the client needs
// in the lobby chat, and judges at the client's done line. The family acts on slot 1 only; another slot idles.

#pragma once

#include <string>

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill::dingus {

enum class Arm { None, Report };

// The arm a config value names; None for a value that is not ours.
Arm ArmOf(const std::string& value);

// The arm's name, for the verdict markers.
const char* ArmName(Arm a);

// Host (judge), game thread: spawn the fixtures, say their keys, judge at the client's done line.
void TickHostJudge(coop::net::Session* s, Arm a);

// Client (sender), game thread: read the keys, send the thirteen messages, say done.
void TickClient(coop::net::Session* s, Arm a);

// Back to the un-armed state (disconnect / re-arm).
void OnDisconnect();

}  // namespace coop::dev::hostile_drill::dingus
