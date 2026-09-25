// coop/dev/door_drill.h -- the door lane's drill, moved by the bot director. The walker (the client,
// or for single player's reading the host with door_drill_host=1) walks to the nearest door its
// navmesh reaches, or the one door_drill_door names, and uses it through the door's own verbs:
//   PRESS    -- presses the closed door; a client's press runs on the host.
//   PRESENCE -- stands in the door's sensor box until the door's next check and reads whether the
//               list the autoclose reads holds the player: from outside stepping in mid-swing, from
//               outside after the swing, and from the approach point (usually listed) after it.
//   CLOSE    -- after each stay, walks fifteen metres away; the autoclose shuts the door within five
//               seconds of its list emptying.
//   HIT      -- a client's pry and hit with nothing held, which the host refuses; a crowbar taken
//               into the hand, one hit past its swing, which the host cuts, then its swings until
//               the door opens; the crowbar stowed and a pry, which the host runs. The host hits at 50.
// EACH peer logs every change in each door's sensor list and open flag, and every begin and end event
// of the walked door's sensor, with the component and the list it left. Tagged [DOOR-DRILL]; run on
// both peers (door_drill=1); the walker's DONE line ends it.

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
