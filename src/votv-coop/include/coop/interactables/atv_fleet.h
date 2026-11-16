// coop/interactables/atv_fleet.h -- a second ATV once the session is large enough to need one.
//
// An ATV seats one player. When the players standing in the host's world reach
// `net.second_atv_players` (5 by default; 0 turns this off) and the world holds one ATV_C, the host
// spawns a second beside the first: at the first of eight spots around it that a line from the ATV
// reaches unblocked, that has ground under it and clear room around it. The spawn is an ordinary runtime ATV, announced
// to the clients on the ATV spawn lane (coop/interactables/atv_sync), and it stays in the host's
// save, so a later session finds two and spawns none. Host only.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::atv_fleet {

// HOST: the check, once every few seconds. Game thread.
void Tick(coop::net::Session* session);

void OnDisconnect();

}  // namespace coop::atv_fleet
