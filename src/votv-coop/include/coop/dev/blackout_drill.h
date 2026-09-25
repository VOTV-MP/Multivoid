// coop/dev/blackout_drill.h -- a blackout on both peers, for the lanes' dev probe to watch. Once a
// client's world is ready the host lights the power panel's groups and shuts the listed doors the
// blackout opens, through their own verbs, and at its next read fires solar through the dev fire's
// seam (coop/world/event_fire_sync; the client replays it): the panel's solar() cuts every breaker,
// turns its groups off and opens each listed door that does not ignore a blackout, each write on its
// lane. Each peer says so once its copy reads all of it; a client whose copy has not by a bound FAILs.
// With blackout_drill_door naming a door the blackout does not open, the host shuts it and, once its
// breakers read cut, makes it inactive if the blackout left it active; the client walks to it with the
// director and presses it: the press runs on the host, where the door must open by hand as single
// player opens it in a blackout (one that ignores a blackout ends INCONCLUSIVE); with
// blackout_drill_control there is no fire, and the inactive door must stay shut. Lines are tagged
// [BLACKOUT-DRILL]; run on both peers (blackout_drill=1) with channel_shadow_probe=1; the client's DONE
// line ends it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::blackout_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off, four reads of the panel's reach a second
// when on.
void Tick(coop::net::Session* session);

// The panel and the phase belong to one world.
void OnDisconnect();

}  // namespace coop::dev::blackout_drill
