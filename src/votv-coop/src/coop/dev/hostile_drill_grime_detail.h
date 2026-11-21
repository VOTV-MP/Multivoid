// coop/dev/hostile_drill_grime_detail.h -- what the grime family's files share: the arm table, the numbers the arms'
// expectations stand on, the verdict print and the lobby-chat lines the two peers say to each other
// (coop/dev/drill_chat.h). Private to coop/dev: hostile_drill_grime*.cpp.

#pragma once

#include "coop/dev/hostile_drill_grime.h"

#include "ue_wrap/core/types.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace coop::dev::hostile_drill::grime::detail {

using Clock = std::chrono::steady_clock;
constexpr auto kFailBound = std::chrono::seconds(240);   // a bound that says INVALID, never a wait that decides
constexpr uint8_t kSlot = 1;                             // the one slot the family acts on

// The classes a held sponge and a wall fixer are found by in a hand record, and the paths the client spawns them from.
constexpr const wchar_t* kSpongeCls = L"prop_sponge_C";
constexpr const wchar_t* kFixerCls = L"prop_wallfixer_C";
constexpr const wchar_t* kSpongePath = L"/Game/objects/prop_sponge.prop_sponge_C";
constexpr const wchar_t* kFixerPath = L"/Game/objects/prop_wallfixer.prop_wallfixer_C";

// The numbers of the game's own verbs the arms expect, written down here from the Blueprint dumps and not taken from
// the judge: a stroke of a sponge with soap falls 1.5 * 2 * 4 * 1.2 = 14.4 of a decal's cleanStrength (prop_sponge dump
// lines 134 and 756-761, grime_C dump lines 220-222), a stroke without soap Sub = 6, and the rain's call
// clean(nullptr, 2, true) falls 2 * 1.2 = 2.4 of it (daynightCycle dump line 973).
constexpr float kSoapFallPerStrength = 14.4f;
constexpr float kRainFallPerStrength = 2.4f;
constexpr float kStrokeSub = 6.0f;
// A decal at least this dirty is fresh: a fall from it is far from its end.
constexpr float kFreshProcess = 90.0f;
// What a far decal is at least from the client's puppet: past the tool's reach and the authority's allowance.
constexpr float kFarMinCm = 3000.0f;
// What the rain's pass could reach of a client: its radius is 10000 (daynightCycle dump line 1609).
constexpr float kWideMaxCm = 9000.0f;
// What `last` leaves the decal at on the host before the client's stroke: one stroke takes it below zero.
constexpr float kLastLeft = 5.0f;
// What the measure arm leaves a crack at on both copies before the wall fixer's use: one use (a flat 10) ends it.
constexpr float kCrackLeft = 5.0f;
constexpr float kValueEps = 0.05f;
// How near the client walks to the decal it will clean: inside the judge's reach, not on top of it.
constexpr float kWalkReachCm = 450.0f;
constexpr int   kWalkDeadlineS = 240;

enum class Pick : uint8_t {
    Near,   // the nearest fresh cleanable decal the sponge cleans
    Far,    // the nearest fresh cleanable decal at least kFarMinCm away
    Any,    // the nearest decal with a key
    Wash,   // the nearest decal the rain washes on the host's copy (it does not resist, is cleanable, no roof), in range
    Wide,   // the nearest decal the rain would not skip by its own flag, in range, roof or no roof
};
enum class Weather : uint8_t { Any, Dry, Raining };

struct Spec {
    const char* name;
    Arm         arm;
    Pick        pick;
    bool        walk;      // the client walks to the decal first
    bool        sponge;    // the client holds a sponge
    Weather     weather;   // what the host's own weather must be (the host sets rain; a dry arm is invalid in rain)
};
const Spec* SpecOf(Arm a);

// The decal one arm acts on: its key, its place and its dirt when the host picked it.
struct Target {
    std::string      key;
    ue_wrap::FVector at{};
    float            process = 0.f;
};
std::string FormatTarget(const Target& t);
bool ParseTarget(const std::string& text, Target& out);

// `<what>` lines of one arm: "hostile-drill <name> <what> ", the prefix a reader matches and takes the rest of.
std::string Line(Arm a, const char* what);

// The verdict line: PASS to the info log, FAIL and INCONCLUSIVE as warnings (`<arm> PASS -- why`, `<arm> FAIL: why`).
void Verdict(Arm a, const char* what, const std::string& why);

void ClientTick(coop::net::Session* s, const Spec& sp);
void ClientReset();
void HostTick(coop::net::Session* s, const Spec& sp);
void HostReset();

}  // namespace coop::dev::hostile_drill::grime::detail
