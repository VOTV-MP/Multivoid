// coop/dev/door_drill.h -- the door lane's drill, moved by the bot director. The walker (the client,
// or the host with door_drill_host=1, single player's reading of the same phases) walks to the
// nearest door its navmesh reaches and uses it as a player does, through the door's own verbs:
//   PRESS    -- presses the closed door; a client's press runs on the host.
//   PRESENCE -- stands at the centre of the door's sensor box and stays while each peer reads
//               whether its own sensor list, the one the autoclose reads, holds the player; twice,
//               stepped into mid-swing and again after a second press's swing has ended.
//   CLOSE    -- after each stay, walks fifteen metres away; the autoclose shuts the door within
//               five seconds of its list emptying.
//   HIT      -- back at the approach point, a held weapon's hits until the pry opens the door.
// EACH peer logs every change in each door's sensor list and open flag. Lines are tagged
// [DOOR-DRILL]. Run on both peers of a pair (door_drill=1); the walker's DONE line ends it.

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
