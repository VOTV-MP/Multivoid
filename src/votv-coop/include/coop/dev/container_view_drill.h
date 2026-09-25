// coop/dev/container_view_drill.h -- [dev] F-108's drill: a player's view into a container closes as the
// actor it was opened through leaves the player's reach, and not before (coop/props/container_view_close).
//   CLIENT -- once its join is over, walks with the director to the ATV its navmesh reaches first, opens the
//             ATV's container through the gamemode's own openPropInv, as the ATV's verb does, then walks
//             back to where it started. Every tick it reads its own view and how far the camera stands past
//             the reach (the arm's length plus the ATV's reach sphere): the view must be open on every tick
//             the ATV is within reach, and closed on the tick after the first one where it is not. A walk
//             that ends with the view still open fails.
//   HOST   -- nothing of its own: the close rule runs on every peer.
// Lines are tagged [CVIEW-DRILL]; run with container_view_drill=1 and join_at_host=1 (the rig's save has one ATV,
// beside the host and far from where a client spawns); the client's DONE line, which carries the verdict, ends it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::container_view_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off. A walk thread of its own, then a view read
// and two engine reads a tick while the client walks away.
void Tick(coop::net::Session* session);

// The ATV, the view and the walks belong to one world and one session.
void OnDisconnect();

}  // namespace coop::dev::container_view_drill
