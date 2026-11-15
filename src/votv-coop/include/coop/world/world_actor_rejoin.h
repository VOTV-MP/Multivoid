// coop/world/world_actor_rejoin.h -- a joiner is shown every host-owned world actor that is still standing.
//
// When the host's last client leaves, the world-actor lane drops every row (world_actor_sync::OnDisconnect), though the host's
// actors live on, so the lane's own connect snapshot has nothing to send the next player to join: a body that waits for a pawn,
// a saucer, a coin on the floor would never be shown to him. At a joiner's world-ready the host walks the live instances of the
// lane's classes and enrols each one the lane does not track (world_actor_sync::HostEnrollExSpawn skips the tracked), which
// sends it to every peer, the joiner among them. The character lane's own walk is npc_world_enum::RegisterExistingWorldNpcs.
// The classes it leaves out, and why each, are the table in the .cpp. Game thread, host only.

#pragma once

namespace coop::world_actor_rejoin {

// The standing actors enrolled for the peer in `joinerSlot`; the count of new rows (0 when the lane already holds every one).
int EnrolStandingForJoiner(int joinerSlot);

}  // namespace coop::world_actor_rejoin
