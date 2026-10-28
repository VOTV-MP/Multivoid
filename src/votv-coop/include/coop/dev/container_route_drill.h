// coop/dev/container_route_drill.h -- [dev] the drill of the container route census: the CLIENT drives each of the
// census's routes once against one shared container the HOST places beside its puppet, with the game's own verbs at
// the human seam, and both peers print the census after every step. Armed by the row container_route_census=drill
// (or drill-before) on BOTH peers; see coop/dev/container_route_census.h.
//   HOST   -- spawns a backpack of two drives and a loose drive beside it, hands it two fresh records once it judged
//             R1's slice, arms R4 by taking the set of live drives, finds and traces the drive its drop path makes
//             for the throw, then opens the backpack's screen, appends a record of a second class, logs whether the
//             screen rebuilt, and writes once more unpublished for the stale take.
//   CLIENT -- R1 a slot press, R2 the take key with no hovered slot, R3 hold+run, then, the screen closed, R4 the
//             held drive thrown at the backpack once the host is armed and the aim struck; then the screen's
//             rebuild, and the refused take.
// Each route is its own step: a bound or a refusal prints a FAIL line of the arm and the drill goes on to the next
// route; only a fixture failure ends it ABANDONED. The peers sequence on drill_chat lines; the client's DONE and the
// host's DONE end the run once every route was tried.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::container_route_drill {

// Advances this peer's steps; a latched read when the census is not in a drill arm. Game thread.
void Tick(coop::net::Session* session);

// The session ended: the drill starts over.
void OnDisconnect();

}  // namespace coop::dev::container_route_drill
