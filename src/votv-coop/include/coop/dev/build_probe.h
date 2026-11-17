// coop/dev/build_probe.h -- [dev] the building probe: what the Blueprint dumps cannot say about the nail, the wall and the
// poster (ini build_probe=on / env VOTVCOOP_BUILD_PROBE on BOTH peers, the peers' folder VOTVCOOP_CAN_DIR, absolute).
// It judges nothing: its lines are the result, and it ends on `build-probe: DONE`.
//   damage -- the HOST, walked by the director to the level's wall, throws a brick, swings an axe and detonates an
//             explosion at it, then the same at a customWall_C put on the ground beside the host; watches log the routes,
//             a throw its nearest approach to the wall's box, a swing what its view trace struck, a leg the addDamage targets;
//             where another actor blocks the view, the host steps to another side of the wall's box (up to 4) first.
//   brick  -- an unfinished wall on each peer, put on the ground at one place and read back, one brick thrown by the
//             client into its own; each peer logs its overlap delegate, its brick count and the brick's nearest approach.
// Off, nothing is registered. Game thread.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::build_probe {

// Every pump tick, any role, with or without a session. A latched read when off.
void Tick(coop::net::Session& session);

// The session ended: the steps and the peers' marks start over. The world records stay.
void OnDisconnect();

}  // namespace coop::dev::build_probe
