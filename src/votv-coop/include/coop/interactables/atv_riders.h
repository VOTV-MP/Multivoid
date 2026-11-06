// coop/interactables/atv_riders.h -- who rides an ATV, shown on every peer. A remote driver's body sits in the seat the
// host's table gives it (coop/player/puppet_seat.h). The kerfurs answer a client driver as they answer the host's: the
// ATV's own sit asks every kerfur to come and the dismount sends the rider off, and on the host that runs only for the
// host's own player, so the host runs the same steps on its copy (ue_wrap::atv::InviteKerfurs, ReleaseKerfurs) when
// a client's seat begins and ends. A kerfur riding on the host rides every client's copy (the NPC pose stream's
// kEntityPoseBitKerfurOnAtv, coop/element/npc). Game thread. Overview: docs/vehicles.md.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::atv_riders {

// Store the session. Idempotent; called every load tick.
void Install(coop::net::Session* session);

// The seats onto the remote bodies, and on the host, a client seat's start and end onto its kerfurs. Game thread.
void Tick();

// The session ended: every seat is let go and the kerfur calls are forgotten.
void OnDisconnect();

}  // namespace coop::atv_riders
