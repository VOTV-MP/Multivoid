// harness/autotest/woodchipper_drill_arms.h -- co-located private header of autotest_woodchipper.cpp and
// autotest_woodchipper_mini.cpp: what the mini arm reads of the woodchipper drill (its readings of the chipper's copy and its
// pour, defined in autotest_woodchipper.cpp) and the arm's own entry points (autotest_woodchipper_mini.cpp).
#pragma once

#include <string>

namespace harness::autotest::woodchipper_drill {

inline constexpr int kSignalSec = 240;                       // a step of the other peer
inline constexpr int kCopySec = 40;                          // a state reaches a copy: the send, the round trip, the apply
inline constexpr float kFuelTol = 0.6f;
inline constexpr const char* kTagMini = "woodchipper-mini";
inline constexpr const wchar_t* kMiniClass = L"prop_gascan_mini_C";   // a subclass of the plain can

// The drill's reads and its pour (autotest_woodchipper.cpp).
float FuelNow(const std::wstring& key);              // this peer's copy of the chipper's fuel
std::string CopyText(const std::wstring& key);       // its reading as the drill signals it, "fuel=.. active=.."
unsigned long long RefusedForCan();                  // the host's count of pours refused for the can
float PourHeldCan(const std::wstring& key);          // the client pours the can in its hand: the fuel its copy gained

// The mini arm (autotest_woodchipper_mini.cpp).
const char* SpawnMiniCan();                          // HOST: a mini can in the world, its class listed here; else the cause it failed
void HostMini(const std::wstring& key);             // HOST: the arm's verdict, 'woodchipper-mini'
void ClientMini(const std::wstring& key);           // CLIENT: the pour of the mini can in the hand

}  // namespace harness::autotest::woodchipper_drill
