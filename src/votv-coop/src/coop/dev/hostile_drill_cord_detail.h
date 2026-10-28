// coop/dev/hostile_drill_cord_detail.h -- what the cord family's files share: the arm table, the counters an arm is
// judged on, the verdict print and the lobby-chat lines the two peers say to each other (coop/dev/drill_chat.h).
// Private to coop/dev: hostile_drill_cord*.cpp.

#pragma once

#include "coop/dev/hostile_drill_cord.h"

#include "coop/interactables/cord_sync.h"

#include <array>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <string>

namespace coop::dev::hostile_drill::cord::detail {

using Clock = std::chrono::steady_clock;
constexpr auto kFailBound = std::chrono::seconds(180);   // a failure bound, never a wait
constexpr uint8_t kSlot = 1;                             // the one slot the family acts on

// The corded prop every arm that needs one spawns: the cord drill's maker, found by its class name.
constexpr const wchar_t* kMakerCls = L"prop_coffeeMaker_C";

// A claim is judged at the reach the lane tests (400 uu): the near maker stands inside it, the far one outside.
constexpr float kNearCm = 120.f;
constexpr float kFarCm = 2000.f;

// The one corded prop an arm needs, and where it stands from the sender.
enum class Fixture : uint8_t { None, Near, Far };

// How the arm runs: Plain sends once the host is ready; Dead has the client kill its own player first, and the host
// says ready only while that player's puppet reads dead.
enum class Mode : uint8_t { Plain, Dead };

// The lane's counters the host judges. Every field not named in an arm's table must stay at 0.
enum class Field : uint8_t {
    Claims, Applied, Equal, Rate, Dead, Reach, HostDrag, Unresolved, KeyPending, LaneOff, Answers,
    DropSize, DropKey, DropOp, DropField, Count
};
constexpr size_t kFields = static_cast<size_t>(Field::Count);
using Tally = std::array<uint32_t, kFields>;
Tally TallyOf(const coop::cord_sync::DrillView& v);
const char* FieldName(Field f);

struct Entry {
    Field    field;
    uint32_t count;
};

// One arm: the messages the client sends (`sent`, the accounting total), the corded prop it needs, and the host's
// expected counter deltas (an entry with count 0 is no entry). The answers the client must see are the arm's `Answers`.
struct Spec {
    const char* name;
    Arm         arm;
    Fixture     fixture;
    Mode        mode;
    uint32_t    sent;
    Entry       want[4];
};
const Spec* SpecOf(Arm a);
Tally Expected(const Spec& sp);

// The verdict line: PASS to the info log, FAIL and INCONCLUSIVE as warnings (`<arm> PASS -- why`, `<arm> FAIL: why`).
void Verdict(Arm a, const char* what, const std::string& why);

// The lobby chat lines the peers say (coop/dev/drill_chat.h): the host's once it is ready, the client's once its start
// set is recorded (which the host spawns the prop after), and the client's count of the answers it saw.
std::string ReadyLine(Arm a);
std::string ClientReadyLine(Arm a);
std::string SawPrefix(Arm a);

void ClientTick(coop::net::Session* s, const Spec& sp);
void ClientReset();
void HostTick(coop::net::Session* s, const Spec& sp);
void HostReset();

}  // namespace coop::dev::hostile_drill::cord::detail
