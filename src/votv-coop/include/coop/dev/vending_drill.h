// coop/dev/vending_drill.h -- [dev] the vending machines, the jammed doors and the Poolwalker as steps of the event-fire drill, reached through coop/dev/gamemode_drill's
// dispatch. No player is moved. Every 5 s each peer says what it holds in `[GAP3]` lines.
//
//   vendBase  the snack/soda machine as it stands: the CLIENT writes balance 1000 on its own copy, picks the first stock item and
//             runs the machine's own actionOptionIndex (the buy); both peers say each machine's stock and how many props they hold.
//   vendLane  the host's machine: the HOST writes balance 1000 on its first machine (the state crosses), the CLIENT then runs the
//             buy on its copy (refused there, asked of the host); the host later empties its stock (written) and runs checkStock,
//             whose beginRestock refills it; the client's copy must show each of those.
//   jamDoor   the Reactor Restart: the host runs trigger_jamDoor_C and its doors' `jammed` flags must reach the client's copies;
//             a client's own run of the trigger is refused.
//   walker    the Poolwalker (poolwalker_C) has no lane: each peer says how many it holds and whether its class is loaded.

#pragma once

#include <cstdint>
#include <string>

namespace coop::dev::vending_drill {

bool IsDrill(const std::string& name);
void ClientStep(const std::string& name);
void HostStep(const std::string& name, uint64_t sinceFireMs);
void OnDisconnect();

}  // namespace coop::dev::vending_drill
