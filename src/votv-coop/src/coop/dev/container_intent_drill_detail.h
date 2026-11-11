// coop/dev/container_intent_drill_detail.h -- what the container intent drill's files share. A private header of
// coop/dev/container_intent_drill*.cpp (src tree, not include/).

#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::container_intent_drill::detail {

// The arms are the throw arms: a client throws a drive at a shared backpack (container_intent_drill_throw*.cpp).
enum class Arm : uint8_t { Off, ThrowHand, ThrowGrab, ThrowHostGrab, ThrowClamp, ThrowPlace, ThrowJoin, ThrowDepart, ThrowTouch };
Arm ArmOf();
const char* ArmName();

// The arms that throw from the hotbar hand: throwclamp is throwhand with a forged velocity.
inline bool IsHandThrow(Arm a) { return a == Arm::ThrowHand || a == Arm::ThrowClamp; }

inline constexpr const wchar_t* kPackPath   = L"/Game/objects/prop_backpack.prop_backpack_C";
inline constexpr const wchar_t* kDriveClass = L"prop_drive_C";
inline constexpr float    kDropCm       = 30.f;
inline constexpr uint64_t kCheckEveryMs = 250;

const char* Role();
void Finish();
void Fail(const char* fmt, ...);

// The record count of the world container `eid`, or -1 when it does not read.
int CountOf(uint32_t eid);

// The element id of an actor, or kInvalidId.
uint32_t EidOf(void* actor);

// The client with a connected client of a lower slot is the second client: the joiner.
bool IsJoiner();

// The integer after "key=" in a chat line's text, or false when the key is absent.
inline bool IntAfter(const std::string& text, const char* key, int& out) {
    const std::string needle = std::string(key) + "=";
    const size_t at = text.find(needle);
    if (at == std::string::npos) return false;
    const char* begin = text.c_str() + at + needle.size();
    char* end = nullptr;
    const long v = std::strtol(begin, &end, 10);
    if (end == begin) return false;
    out = static_cast<int>(v);
    return true;
}

// The throwdepart arm: the parked client leaves a mark (the wall clock) in the mod's data folder; a relaunched client finds
// it, removes it, and is idle when the mark is under 300 s old (the arm runs once per host session). An older mark is
// removed and the arm runs.
void DepartMarkParked();
bool DepartRejoined();

// The throw arms' two halves, and their state back to the start (container_intent_drill_throw.cpp, _throw_host.cpp).
void ThrowHostTick(const coop::net::Session& session);
void ThrowClientTick(const coop::net::Session& session);
void ThrowReset();

}  // namespace coop::dev::container_intent_drill::detail
