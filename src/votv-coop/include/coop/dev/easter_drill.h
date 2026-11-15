// coop/dev/easter_drill.h -- [dev] the Easter hunt as a step of the event-fire drill (`easterHunt`), reached through
// coop/dev/gamemode_drill's dispatch.
//
// The host, seconds after the fire: at 8 calls its zone's playAnimation, as the zone's tick does at seven carrots; at 100
// destroys its bunny (the game then mints the angry one) and at 108 destroys the angry bunny, the eggs and the ears the hunt
// left (the list watches 115 s). The client, counted from the moment it holds a bunny copy: at 6 s calls `fin` on the copy
// (the first talk's end), at 16 s uses one egg copy on it (`playerUsedOn`), at 24 s calls `CustomEvent` (a later talk's end);
// each must be refused there and run by the host. The hunt is never finished, so no achievement is granted to this machine's
// profile. No player is moved. Every 5 s each peer says in `[EASTER-DRILL]` lines what it holds.

#pragma once

#include <cstdint>
#include <string>

namespace coop::dev::easter_drill {

bool IsDrill(const std::string& name);
void ClientStep();
void HostStep(uint64_t sinceFireMs);
void OnDisconnect();

}  // namespace coop::dev::easter_drill
