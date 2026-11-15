// coop/world/trash_fling_sync.h -- the trash prank's flung props and its garbage clumps reach the clients.
//
// The prank `trashBase` spawns arirTrasher_C. Once a second, while the host's player is within 50 m of its box, its
// `trash` timer function does up to ten times: pick a random prop overlapping the box, add a horizontal velocity
// of 2.5 to 10 m/s and a spin to it, and spawn a prop_garbageClump_C beside it with a 10 m/s velocity. The flung
// props are not births, hooks or broom pushes, so no lane moved their copies; the clumps are plain actors the
// finish-spawn seam leaves to the census, about 20 s on. A script-gate post watch on `trash` notes the box's
// reach; the next tick coasts every simulating prop inside it that is moving fast (the drive streams it to rest)
// and announces each clump with no element id yet: its spawn row, then its roll on the trash carry lane. Host only.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::trash_fling_sync {

// Both roles, every pump tick: registers the watch on arirTrasher_C::trash until the gate takes it.
void Install(coop::net::Session* s);

// Host, game thread, every gameplay tick, before prop_drive_host::Pump. Returns at once when no `trash` call ran.
void Tick(coop::net::Session* s);

// Forgets the calls not yet read.
void OnDisconnect();

}  // namespace coop::trash_fling_sync
