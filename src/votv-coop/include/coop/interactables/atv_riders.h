// coop/interactables/atv_riders.h -- who rides an ATV, shown on every peer. A remote driver's body sits in the seat the
// host's table gives it (coop/player/puppet_seat.h). The kerfurs answer a client driver as they answer the host's: the
// ATV's own sit asks every kerfur to come and the dismount sends the rider off, and on the host that runs only for the
// host's own player, so the host runs the same steps on its copy (ue_wrap::atv::InviteKerfurs, ReleaseKerfurs) on the
// seat table's edge when a client's seat begins and ends; an ATV destroyed under kerfurs it invited turns them back. A
// kerfur riding on the host rides every client's copy: the ATV's stamp names it (riderEid) and coop/creatures/npc_ride
// seats the mirror. All of it is the seat table's subscriber, nothing polls. Game thread. Overview: docs/vehicles.md.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::atv_riders {

// Store the session and register the subscribers. Idempotent; called every load tick.
void Install(coop::net::Session* session);

// The session ended: the body seats are dropped and the tallies are said.
void OnDisconnect();

}  // namespace coop::atv_riders
