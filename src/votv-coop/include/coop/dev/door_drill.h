// coop/dev/door_drill.h -- the door lane's drill, moved by the bot director. Its first phase,
// PRESENCE, answers the question the door migration rests on: does a remote player's puppet
// count in a door's own sensor, the list the game's autoclose reads? Two claims in the tree say
// it does not (the host hold register in coop/interactables/interactable_channel.h, the client
// suppression in ue_wrap/devices/door.h), and neither was measured.
//
// The CLIENT walks with the director to an approach point of the nearest door its navmesh route
// reaches, stays there, then walks fifteen metres back along its route; a closed door on the way
// opens as a player's press would (a client's request to the host). EACH peer reads every
// door's sensor list and logs each change in how many puppets and local players it holds; the
// host's reading of the walking client is the measured direction. Lines are tagged [DOOR-DRILL].
// Run on both peers of a pair (door_drill=1); the client's "[DOOR-DRILL] client DONE" ends it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::door_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off. The sensor readings on either
// peer, four a second, and on a client the walker's one start once its world is ready.
void Tick(coop::net::Session* session);

// The door list and the readings belong to one world.
void OnDisconnect();

}  // namespace coop::dev::door_drill
