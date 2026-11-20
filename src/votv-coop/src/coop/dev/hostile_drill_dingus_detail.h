// coop/dev/hostile_drill_dingus_detail.h -- what the dingus_report arm's files share: the arm table, the counters the arm is
// judged on, the verdict print and the lobby-chat lines the two peers say to each other (coop/dev/drill_chat.h). Private to
// coop/dev: hostile_drill_dingus*.cpp.

#pragma once

#include "coop/dev/hostile_drill_dingus.h"

#include "coop/props/dingus_sync.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::dev::hostile_drill::dingus::detail {

using Clock = std::chrono::steady_clock;
constexpr auto kBound = std::chrono::seconds(180);   // a bound says INCONCLUSIVE, never a verdict
constexpr uint8_t kSlot = 1;                         // the one slot the family acts on

// A report is judged at 600 cm: the far dingus stands outside it.
constexpr float kFarCm = 2000.f;

// The counters the host judges, every one a delta since the baseline: the dispatcher's drops, the judge's refusals by reason,
// its acceptances and the found set's size.
enum class Field : uint8_t {
    Dropped, NotReady, Dead, Budget, Known, NoCopy, NotDingus, Unreachable, Accepted, FoundKeys,
    Count
};
constexpr size_t kFields = static_cast<size_t>(Field::Count);
using Tally = std::array<uint32_t, kFields>;
Tally TallyOf(const coop::dingus_sync::dev::JudgeView& v, size_t foundKeys);
const char* FieldName(Field f);

struct Entry {
    Field    field;
    uint32_t count;
};

// The arm: the messages the client sends (`sent`, the accounting total) and the host's expected counter deltas (an entry with
// count 0 is no entry).
struct Spec {
    const char* name;
    Arm         arm;
    uint32_t    sent;
    Entry       want[5];
};
const Spec* SpecOf(Arm a);
Tally Expected(const Spec& sp);

// The verdict line: PASS to the info log, FAIL and INCONCLUSIVE as warnings (`<arm> PASS -- why`, `<arm> FAIL: why`).
void Verdict(Arm a, const char* what, const std::string& why);

// The lobby chat lines (coop/dev/drill_chat.h): the host's keys, the plain prop's and the far dingus's, and the client's done.
std::string KeyLine(Arm a, const char* label);
std::string DoneLine(Arm a);

void ClientTick(coop::net::Session* s, const Spec& sp);
void ClientReset();
void HostTick(coop::net::Session* s, const Spec& sp);
void HostReset();

}  // namespace coop::dev::hostile_drill::dingus::detail
