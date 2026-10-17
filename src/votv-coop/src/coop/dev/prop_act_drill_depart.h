// coop/dev/prop_act_drill_depart.h -- the depart phase's step and its lines, read by both halves in
// prop_act_drill_depart.cpp. Private to the act drill.

#pragma once

#include "coop/dev/prop_act_drill.h"

#include <cstdint>
#include <string>

namespace coop::dev::prop_act_drill::detail {

// The first life's request id: the newcomer's honest request gets the same id (a fresh process counts from nothing),
// so its forged commit names exactly the reservation the departed life left.
inline constexpr uint16_t kLife1RequestId = 1;

// The one step: the target is a crowbar, c1 pockets it.
inline const Row kDepartRow{"depart", kCrowbar, L"prop_crowbar_C", Verb::Pocket, Ends::Inventory, nullptr};

// The line the rig's --rejoin-marker waits for: life 1 holds its commit, so the host holds its reservation.
inline std::string LifeOneMarker() { return std::string(kTag) + " c1 depart LIFE 1 DONE"; }
// The host, once slot 1 is world-ready again: the newcomer's id and the target's eid.
inline std::string NewcomerHead() { return std::string(kTag) + " host depart newcomer id=" + std::to_string(kLife1RequestId) + " eid="; }
// The host, once the forged commit was tallied bad and the target stands.
inline std::string BadSeenLine() { return std::string(kTag) + " host depart bad commit seen"; }

}  // namespace coop::dev::prop_act_drill::detail
