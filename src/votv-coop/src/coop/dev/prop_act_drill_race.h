// coop/dev/prop_act_drill_race.h -- the race phase's step table and what its host half (prop_act_drill_race.cpp) and
// its client half (prop_act_drill_race_client.cpp) share. Private to the act drill.

#pragma once

#include "coop/dev/prop_act_drill.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::dev::prop_act_drill::detail {

inline constexpr int kSlotC1 = 1, kSlotC2 = 2;

// Who acts on the target after c1's act. C2Takes: c1 makes the target and c2 takes it.
enum class Blocker : uint8_t { C2Pocket, HostActs, D1Remove, C2Grab, C2Hook, HostGrab, C2Takes };

struct RaceRow {
    Row act;   // the target and c1's act; `ends` is what c1's own copies show when the take is granted
    Blocker blocker;
};

extern const RaceRow kRaceRows[];
extern const size_t kRaceRowCount;

// c1's commit is held from its grant until the others have been answered.
inline bool C1Holds(Blocker b) { return b == Blocker::C2Pocket || b == Blocker::HostActs || b == Blocker::D1Remove; }
// The take is refused as held, so c1 keeps nothing.
inline bool C1Refused(Blocker b) { return b == Blocker::C2Grab || b == Blocker::C2Hook || b == Blocker::HostGrab; }
inline bool C2SaysDone(Blocker b) { return b == Blocker::C2Pocket || b == Blocker::C2Takes; }

// The lines the host says at a step's waypoints, and the line c1 says its disc's key in.
inline std::string HostLine(const RaceRow& r, const char* what) { return std::string(kTag) + " host " + r.act.name + " " + what; }
inline std::string KeyHead() { return std::string(kTag) + " c1 other spawn key="; }

inline std::string Narrow(const std::wstring& w) {
    std::string s;
    for (const wchar_t c : w) s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return s;
}

inline uint32_t Since(uint32_t now, uint32_t then) { return now - then; }

// The client half's state back to the start (the host half's reset calls it).
void RaceClientReset();

}  // namespace coop::dev::prop_act_drill::detail
