// coop/creatures/creature_hit.h -- what a creature is, for the code that judges a hit on a player.
//
// The one place that knows it: an actor is a creature when the element registry names it an Npc element -- the real
// creature on the host, its mirror on a client. coop/player/player_damage asks it for the caller of a hit and
// delivers only a hit whose creature's family is parked (coop/creatures/npc_mirror_brain.h): that family's mirror
// makes no hit of its own, so the host's hit is the only one. A family whose mirror still hits keeps today's pair of
// hits and is never delivered. Game thread.

#pragma once

namespace coop::creature_hit {

// Is `actor` a creature: an actor whose element is an Npc. False for null and for an actor with no element.
bool IsCreature(void* actor);

// Is `actor` a creature of a parked family (npc_mirror_brain::IsParkedClass of its class)?
bool IsDelivered(void* actor);

}  // namespace coop::creature_hit
