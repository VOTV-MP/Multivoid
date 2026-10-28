// coop/dev/hostile_drill_coffee_detail.h -- what the coffee family's files share: the arm table, the counters an arm is
// judged on, the verdict print and the lobby-chat lines the two peers say to each other (coop/dev/drill_chat.h).
// Private to coop/dev: hostile_drill_coffee*.cpp.

#pragma once

#include "coop/dev/hostile_drill_coffee.h"

#include "coop/interactables/coffee_intent.h"
#include "coop/interactables/coffee_state.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::dev::hostile_drill::coffee::detail {

using Clock = std::chrono::steady_clock;
constexpr auto kFailBound = std::chrono::seconds(180);   // a failure bound, never a wait
constexpr uint8_t kSlot = 1;                             // the one slot the family acts on

// The classes an arm's prop is found by on the client: the coffee maker, and the crowbar of the wrong-class arm.
constexpr const wchar_t* kMakerCls = L"prop_coffeeMaker_C";
constexpr const wchar_t* kCrowbarCls = L"prop_crowbar_C";

// A press is judged at the reach the lane tests (400 uu): the near prop stands inside it, the far one outside.
constexpr float kNearCm = 120.f;
constexpr float kFarCm = 2000.f;

// How the arm runs: Plain sends once the host is ready; Dead has the client kill its own player first, and the host
// says ready only while that player's puppet reads dead.
enum class Mode : uint8_t { Plain, Dead };

// The one prop an arm needs, and where it stands from the sender.
enum class Fixture : uint8_t { None, NearMaker, FarMaker, NearCrowbar };

// The lane's counters the host judges. Every field not named in an arm's table must stay at 0.
enum class Field : uint8_t {
    Claims, Ran, NoDevice, WrongClass, OutOfReach, NotRun, QueueFull, SenderDead, DropSize, DropKey, MadeMaker,
    RecordsRefused,
    Count
};
constexpr size_t kFields = static_cast<size_t>(Field::Count);
using Tally = std::array<uint32_t, kFields>;
Tally TallyOf(const coop::coffee_intent::DrillView& v, const coop::coffee_state::DrillView& st);
const char* FieldName(Field f);

struct Entry {
    Field    field;
    uint32_t count;
};

// One arm: the messages the client sends (`sent`, the accounting total), the prop it needs, and the host's expected
// counter deltas (an entry with count 0 is no entry).
struct Spec {
    const char* name;
    Arm         arm;
    Fixture     fixture;
    Mode        mode;
    uint32_t    sent;
    Entry       want[5];
};
const Spec* SpecOf(Arm a);
Tally Expected(const Spec& sp);

// The class a fixture's prop is of, and a name for the lines.
const wchar_t* FixtureClass(Fixture f);

// The verdict line: PASS to the info log, FAIL and INCONCLUSIVE as warnings (`<arm> PASS -- why`, `<arm> FAIL: why`).
void Verdict(Arm a, const char* what, const std::string& why);

// The lobby chat lines the peers say (coop/dev/drill_chat.h): the host's once it is ready, and the client's once its
// start set is recorded (which the host spawns the prop after).
std::string ReadyLine(Arm a);
std::string ClientReadyLine(Arm a);

void ClientTick(coop::net::Session* s, const Spec& sp);
void ClientReset();
void HostTick(coop::net::Session* s, const Spec& sp);
void HostReset();

}  // namespace coop::dev::hostile_drill::coffee::detail
