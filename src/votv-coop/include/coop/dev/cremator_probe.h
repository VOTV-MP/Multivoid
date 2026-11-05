// coop/dev/cremator_probe.h -- [dev] what the cremator Blueprint's own burn does to a coop player.
//   CLIENT -- once the world is settled: stands the local pawn in the nearest cremator's firebox with the same
//             placement a host teleport applies, shuts the door if it is open, pulls the lever on its own copy
//             (no lane), and logs one line a second: health, burn time, the machine's flags and windup, the door,
//             the ragdoll's dead bit and the pawn's position. At 40 s alive it pulls the lever again (does the
//             run stop through the handle); it ends at the death plus ten more lines, or at 90 s alive.
//   Nothing on the host. It MUTATES the client's world.
// Lines: "cremator-probe: PLACED|LEVER|T=...|PULLBACK|DIED|SURVIVED|FAIL|DONE".
// Run: --config cremator_probe=1 (or VOTVCOOP_CREMATOR_PROBE=1) --done-marker 'cremator-probe: DONE'

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::cremator_probe {

// Once per session (the pump's install).
void Install(coop::net::Session* session);

// Game thread, once per pump tick; a single latched read when off.
void Tick();

// Back to the start: a rejoin's client runs the probe again.
void OnDisconnect();

}  // namespace coop::dev::cremator_probe
