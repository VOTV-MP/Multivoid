// coop/dev/door_drill.h -- the door lane's drill, moved by the bot director. The CLIENT walks to an
// approach point of the nearest door its navmesh route reaches and uses it as a player does, through
// the door's own entry verbs, which on a client become verbs the host runs
// (coop/interactables/door_verb_intent):
//   PRESS    -- presses the closed door; the host opens its copy and this copy follows.
//   PRESENCE -- stands at the centre of the door's sensor box and stays, while each peer reads
//               whether its own sensor list -- the one the game's autoclose reads -- holds the
//               player: the host's copy for the client's puppet, the client's for its own player.
//   CLOSE    -- walks fifteen metres back along its route; the host's autoclose shuts the door
//               within five seconds of its sensor emptying, and this copy follows.
//   HIT      -- walks back to the approach point and hits the closed door with a held weapon's
//               damage until the host's pry opens it; this copy moves only when the host's state
//               lands.
// EACH peer reads every door's sensor list and open flag and logs each change. Lines are tagged
// [DOOR-DRILL]. Run on both peers of a pair (door_drill=1); the client's "[DOOR-DRILL] client
// DONE" ends it.

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
