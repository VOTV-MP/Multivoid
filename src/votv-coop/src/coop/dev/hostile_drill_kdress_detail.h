// coop/dev/hostile_drill_kdress_detail.h -- what the kdress_ask arm's files share: the phase table, the counters a phase is
// judged on, the verdict print and the lobby-chat lines the two peers say to each other (coop/dev/drill_chat.h). Private to
// coop/dev: hostile_drill_kdress*.cpp.

#pragma once

#include "coop/dev/hostile_drill_kdress.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::dev::hostile_drill::kdress::detail {

using Clock = std::chrono::steady_clock;
constexpr auto kBound = std::chrono::seconds(180);   // a bound says INCONCLUSIVE, never a verdict
constexpr uint8_t kSlot = 1;                         // the one slot the family acts on

constexpr float kNearCm = 150.f;   // the Omega the asks name stands this far before the client's puppet (reach is 200)
constexpr float kFarCm = 1500.f;   // the Omega out of reach

// The rows the asks name, and the outfit the host gives the near Omega before the take-offs: the glasses (the hold), a second
// head accessory the hand never held, and an outfit.
inline constexpr const wchar_t* kGlasses = L"kerfoacc_glasses";
inline constexpr const wchar_t* kBucket = L"bucket";
inline constexpr const wchar_t* kMaid = L"kerfoout_maid";

// The counters the host judges, every one a delta since the phase's baseline: the host's answers by reason, the tallies of the
// Omega families' terms, the commits taken, and the pat judge's outcomes.
enum class Field : uint8_t {
    Granted, Gone, WrongFamily, Held, Reach, QueueFull,
    FitArg, FitRule, FitHand, FitBudget, FitUnread, FitBadFamily,
    CommitApply, CommitAbort, CommitBad, CommitGone,
    PatDropped, PatAccepted, PatNotReady, PatDead, PatBudget, PatNoOmega, PatUnreachable, PatNoPat,
    Count
};
constexpr size_t kFields = static_cast<size_t>(Field::Count);
using Tally = std::array<uint32_t, kFields>;
Tally Now();
const char* FieldName(Field f);

struct Entry {
    Field    field;
    uint32_t count;
};

// One phase: the host's expected counter deltas (a field not named stays 0).
struct Phase {
    const char* name;
    Entry       want[8];
};
inline constexpr int kPhases = 3;
const Phase& PhaseAt(int i);
Tally Expected(const Phase& p);

// The verdict line: PASS to the info log, FAIL and INCONCLUSIVE as warnings (`<arm> PASS -- why`, `<arm> FAIL: why`).
void Verdict(const char* what, const std::string& why);

// The lobby chat lines (coop/dev/drill_chat.h), each led by a tag no save can hold.
std::string Line(const char* what);

void ClientTick(coop::net::Session* s);
void ClientReset();
void HostTick(coop::net::Session* s);
void HostReset();

}  // namespace coop::dev::hostile_drill::kdress::detail
