// coop/dev/hostile_drill_act_detail.h -- sibling-internal state shared by the act drill arms' two TUs
// (hostile_drill_act.cpp, the router and the single-target arms; hostile_drill_act_family.cpp, the act_wrongfamily arm).
// The remover_drill_detail.h shape: private to the src tree, never part of the include surface. The ignite family's host
// half (hostile_drill_ignite_host.cpp) includes it too, for SpawnOnFloor, and the cord family's (hostile_drill_cord_host.cpp)
// for SpawnClassOnFloor.

#pragma once

#include "coop/dev/hostile_drill_act.h"
#include "coop/element/element.h"
#include "coop/net/protocol.h"

#include <cstdint>
#include <string>

namespace coop { class RemotePlayer; }
namespace coop::net { class Session; }

namespace coop::dev::hostile_drill::act::detail {

inline constexpr const wchar_t* kCrowbarPath = L"/Game/objects/prop_crowbar.prop_crowbar_C";

// The counter movement since a baseline, never below 0.
inline uint32_t Since(uint32_t now, uint32_t base) { return now >= base ? now - base : 0; }

// The lobby chat the two peers say their lines in; a feed walk is a few times a second (PollDue).
void Say(const std::string& line);
bool ChatLine(const std::string& needle, std::string* whole);
bool PollDue();

// This client's own join is over: its world is announced ready and the join has no phase running.
bool OwnJoinOver();

// A PropActRequest for `eid` to the host with the forged id `id`.
bool SendRequest(coop::net::Session* s, uint16_t id, coop::net::PropActFamily family, coop::element::ElementId eid);

// A verdict line: a PASS is info, any other word a warning.
void LogVerdict(Arm a, const char* what, const std::string& why);

// Spawns the class at `path` on the first floor found round the puppet, trying the fan's directions from `firstDir`
// on (3000 uu out when `far`, else beside it); `*dirUsed`, when given, is the direction taken. Null on success, else
// why not.
const char* SpawnOnFloor(coop::RemotePlayer& rp, const wchar_t* path, bool far, int firstDir, int* dirUsed, void** out);

// The same for a class already in hand, `outCm` out from the puppet; `far` picks the trace's start height and depth (the
// far values reach down from 1500 uu above the puppet, the near ones from 150).
const char* SpawnClassOnFloor(coop::RemotePlayer& rp, void* cls, float outCm, bool far, int firstDir, int* dirUsed,
                              void** out);

// The directions SpawnOnFloor tries.
inline constexpr int kFan = 8;

// act_wrongfamily (hostile_drill_act_family.cpp): each peer's half, and its state back to the start.
void TickFamilyClient(coop::net::Session* s);
void TickFamilyHost(coop::net::Session* s);
void ResetFamily();

}  // namespace coop::dev::hostile_drill::act::detail
