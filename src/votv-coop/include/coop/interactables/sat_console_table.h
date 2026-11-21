// coop/interactables/sat_console_table.h -- which machine runs a line typed at the SAT console.
// A command whose effect or answer rests on the shared world runs on the host; one whose effect is
// the typist's own -- its compass, its terminal, its desk's presentation, its machine -- runs where it
// was typed, as does an unknown command's "err". The line is split as enterCommand splits it: the
// command is everything before the first space (the whole line when there is none, or when the
// space leads), a `debug` row is named by the rest, and every name compares without case, as the
// game's switches do. Both ends read the same table: the typist to decide what it sends, the host to
// run only what a typist may send.
#pragma once

#include <string>

namespace coop::sat_console_table {

enum class Runs { Here, OnHost };

Runs Classify(const std::wstring& line);

// A host-run command that spawns into the shared world on every call, with no limit of its own (a gift
// box, a rufus, a thiccfus): the host holds a typist to its own budget of these.
bool SpawnsEveryCall(const std::wstring& line);

// Whether `line` may not start at one console while another console runs `running`: a long command that writes
// what the other one writes. sd.calall writes every dish, so it waits for every other calibration; sd.cal writes
// its own console's dish, so it waits only for an sd.calall. Every other command runs at each console on its own:
// a read, one that finishes at once, or one whose effect is its console's own server (sv.request, sv.eject).
bool Conflicts(const std::wstring& line, const std::wstring& running);

// The command of a line, as enterCommand splits it.
std::wstring CommandOf(const std::wstring& line);

}  // namespace coop::sat_console_table
