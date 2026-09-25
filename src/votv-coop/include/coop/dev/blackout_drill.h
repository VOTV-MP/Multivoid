// coop/dev/blackout_drill.h -- a blackout on both peers, for the lanes' dev probe to watch. The host
// fires the solar event once a client's world is ready, through the dev fire's own seam
// (coop/world/event_fire_sync's HostFire; the client replays it): the power panel's solar() cuts
// every breaker, turns the panel's light groups off through their runTrigger and opens the doors
// wired to open in a blackout. Each peer says DONE once its own panel reads every breaker cut; a
// client whose panel has not by a bound says so. Lines are tagged [BLACKOUT-DRILL]; run on both
// peers (blackout_drill=1), with channel_shadow_probe=1 for the measurement; the client's DONE line
// ends it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::blackout_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off, four panel reads a second when on.
void Tick(coop::net::Session* session);

// The panel and the phase belong to one world.
void OnDisconnect();

}  // namespace coop::dev::blackout_drill
