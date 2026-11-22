// coop/dev/hostile_drill_bucket.h -- the bucket_ask drill arm, proving the host refuses what a client's forged asks for the bucket
// verbs may not do and counts each refusal under its own reason. Defensive testing of OUR OWN mod, per coop/dev/hostile_drill.h;
// nothing here ships enabled.
//
// A joined client holds a cement bag honestly, then forges PropActRequest asks for the bucket families (a soap amount of 99, a mix
// need the host's copy does not give, a transfer direction 2, a mix aimed at a prop of another kind, then the honest mix, granted and
// reserving its target; a second honest mix is sent under that reservation and waits, the host saying so, and is answered after the
// client aborts the first), swaps the bag for a sponge, and forges a dip branch the host's
// soap contradicts. It then swaps the sponge for a second bag the host staged with 5 units and asks a mix that count cannot pay, sends
// the host a save record for the first bag's key that no drop awaits, and asks the mix again: both are refused. The asks go one at a
// time, each after the host's reply to the one before, and the client checks each reply. The
// HOST judges by accounting: the exact deltas of the prop_act counters per phase. The table is in hostile_drill_bucket.cpp. The red is
// bucket_red=judge, which makes each family skip its own check: the first forged ask is granted and the client says FAIL at that reply.
// The host stages one filled bucket, two cement bags and two sponges before the client's puppet with the bucket drill's stage. The
// family acts on slot 1 only; another slot idles.

#pragma once

#include <string>

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill::bucket {

enum class Arm { None, Ask };

// The arm a config value names; None for a value that is not ours.
Arm ArmOf(const std::string& value);

// The arm's name, for the verdict markers.
const char* ArmName(Arm a);

// Host (judge), game thread: stage the fixtures, judge each phase at the client's line.
void TickHostJudge(coop::net::Session* s, Arm a);

// Client (sender), game thread: bind the fixtures, hold the items, send the phases.
void TickClient(coop::net::Session* s, Arm a);

// Session end: back to the first step.
void OnDisconnect();

}  // namespace coop::dev::hostile_drill::bucket
