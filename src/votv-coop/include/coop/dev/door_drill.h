// coop/dev/door_drill.h -- the door lane's drill, moved by the bot director. The walker (the client,
// or for single player's reading the host with door_drill_host=1) walks to the nearest door its
// navmesh reaches, or the one door_drill_door names, and uses it through the door's own verbs:
//   PRESS    -- presses the closed door; a client's press runs on the host.
//   PRESENCE -- stands in the door's sensor box until its next check and reads whether the autoclose's
//               list holds the player: from outside mid-swing, after the swing, from the approach.
//   CLOSE    -- after each stay, walks fifteen metres away; the door shuts within five seconds.
//   HIT      -- (a client with an empty hand) a pry and a hit, refused; a crowbar in hand, one hit past
//               its swing, cut, its swings until the door opens, then a pry, run. The host hits at 50.
//   AIMED    -- (a client, the door shut again) the camera turned until the game's trace strikes a leaf,
//               then the frame, each pressed through useSelectedAction: each press goes to the host.
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
