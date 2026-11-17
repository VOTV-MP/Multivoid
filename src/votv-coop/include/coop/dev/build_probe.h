// coop/dev/build_probe.h -- [dev] the building probe: what the Blueprint dumps cannot say about the nail, the wall and the
// poster (ini build_probe=on / env VOTVCOOP_BUILD_PROBE on BOTH peers, the peers' folder VOTVCOOP_CAN_DIR, absolute).
// It judges nothing: its lines are the result, and it ends on `build-probe: DONE`.
//   census -- at each world-ready edge of this peer: every live nail, wall, unfinished wall and poster, one line each
//             (class, key, owning level, place, the prop key index's hold and when it began, the join sweep's claim),
//             a summary line, and a diff of the keys against the census of the world before.
//   damage -- the HOST, walked by the director to the level's wall, throws a brick at it, swings an axe at it and
//             detonates an explosion beside it; watches on the wall's damage bodies log the routes and their callers.
//   brick  -- an unfinished wall on each peer at one place, one brick thrown by the client into its own; each peer
//             logs whether the wall's overlap delegate fired on its copy and its brick count before and after.
//   keys   -- with no session, one save at the first world edge; the tree's reload scenario then prints the second census.
// Off, nothing is registered. Game thread.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::build_probe {

// Every pump tick, any role, with or without a session. A latched read when off.
void Tick(coop::net::Session& session);

// The session ended: the steps and the peers' marks start over. The world records stay.
void OnDisconnect();

}  // namespace coop::dev::build_probe
