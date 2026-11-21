// coop/dev/hostile_drill_kdress.h -- the kdress_ask drill arm, proving the host refuses what a client's forged asks for an
// Omega's outfit may not do and counts each refusal under its own reason. Defensive testing of OUR OWN mod, per
// coop/dev/hostile_drill.h; nothing here ships enabled.
//
// A joined client holds the glasses honestly, then forges PropActRequest asks for the families OmegaPutOn and OmegaTakeOff
// (a row no table has, a row the hand never held, an Omega out of reach, the same hold put on twice, a take-off flood past
// its drop budget) and KerfurDressIntent pats (a short payload, an Omega out of reach, a flood past the pat budget). The asks
// go one at a time, each after the host's reply to the one before, and the client checks each reply; the pats go in one tick.
// The HOST judges by accounting: the exact deltas of the prop_act and pat counters per phase, and what the Omega wears. The
// table is in hostile_drill_kdress.cpp. The arm names the lane's own symbols, as a new kind must: its red is a throwaway build
// of the lane whose Omega judge accepts every ask, never landed.
// The host spawns two Omegas (one within the lane's reach of the client's puppet, one out of it) and the glasses, and says
// their eids in the lobby chat. The family acts on slot 1 only; another slot idles.

#pragma once

#include <string>

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill::kdress {

enum class Arm { None, Ask };

// The arm a config value names; None for a value that is not ours.
Arm ArmOf(const std::string& value);

// The arm's name, for the verdict markers.
const char* ArmName(Arm a);

// Host (judge), game thread: spawn the fixtures, say their eids, judge each phase at the client's line.
void TickHostJudge(coop::net::Session* s, Arm a);

// Client (sender), game thread: read the eids, hold the glasses, send the phases.
void TickClient(coop::net::Session* s, Arm a);

// Back to the un-armed state (disconnect / re-arm).
void OnDisconnect();

}  // namespace coop::dev::hostile_drill::kdress
