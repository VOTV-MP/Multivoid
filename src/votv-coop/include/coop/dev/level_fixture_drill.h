// coop/dev/level_fixture_drill.h -- [dev] the level's wandering fixtures, the Magnet Skeleton's stain and the Basalt Pillars, as one
// step of the event-fire drill (`levelGaps`), reached through coop/dev/gamemode_drill's dispatch.
//
// The host, seconds after the fire: at 10 moves its church, booth and siren pole 15 m and shows each (as the game's own move
// does, by writing the actor); at 14 runs the level's blood-skeleton trigger; at 18 stands a buried basalt beside its player
// and at 22 digs it; at 30 writes `a` 0.5 on its pillars; at 45 stands a second buried basalt, which the CLIENT digs on its own
// copy once it has one (the dig must reach the host); at 60 moves the church again. No player is moved. Every 5 s each peer
// says in `[GAP-DRILL]` lines where each fixture stands and how many stains, buried basalts and pillars it holds.

#pragma once

#include <cstdint>
#include <string>

namespace coop::dev::level_fixture_drill {

bool IsDrill(const std::string& name);
void ClientStep();
void HostStep(uint64_t sinceFireMs);
void OnDisconnect();

}  // namespace coop::dev::level_fixture_drill
