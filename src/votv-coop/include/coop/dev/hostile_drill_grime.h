// coop/dev/hostile_drill_grime.h -- the grime_* drill arms, proving the host's judge of a client's grime fall
// (coop/interactables/grime_judge) refuses what a client's cleaning may not do, grants and cuts what it may, and holds
// the last stroke at its source. Defensive testing of OUR OWN mod, per coop/dev/hostile_drill.h; nothing here ships
// enabled.
//
// The host picks the decal each arm needs from the client's puppet and says it in the lobby chat; the client walks to it
// when the arm needs it near (the bot director, never a teleport), spawns and holds a sponge when the arm needs one, and
// says it is ready; the host checks what its own record holds (the sponge in the client's hand, the weather the arm needs)
// and says it is ready; the client then sends the arm's message, a raw GrimeState through the session's own send, or a
// real clean call with the sponge for `last` and `measure`. The HOST judges by the judge's counters and by the value it
// holds for the decal: a refused fall leaves both the decal and the value a joiner's snapshot would carry as they were.
//   grime_measure  grime_last  grime_far  grime_notool  grime_gone  grime_fast  grime_rain  grime_dry  grime_joinleak
// grime_joinleak has two lives: the client leaves after the host judged the refused ask and rejoins (VOTVCOOP_GRIME_DRILL_REJOIN=1).
// The table of what each does is in hostile_drill_grime.cpp. The family acts on slot 1 only; another slot idles.

#pragma once

#include <string>

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill::grime {

enum class Arm { None, Measure, Last, Far, Notool, Gone, Fast, Rain, Dry, Joinleak };

// The arm a config value names; None for a value that is not one of ours.
Arm ArmOf(const std::string& value);

// The arm's name, for the verdict markers.
const char* ArmName(Arm a);

// Host (judge), game thread: pick the decal, say it, prepare what the arm needs, judge and print PASS / FAIL /
// INCONCLUSIVE.
void TickHostJudge(coop::net::Session* s, Arm a);

// Client (sender), game thread: read the host's pick, walk, equip, say ready, act, report.
void TickClient(coop::net::Session* s, Arm a);

// Back to the un-armed state (disconnect / re-arm).
void OnDisconnect();

}  // namespace coop::dev::hostile_drill::grime
