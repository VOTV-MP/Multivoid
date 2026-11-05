// coop/dev/cremator_probe.h -- [dev] what the cremator Blueprint's own burn does to a coop player (MUTATES the world).
//   =1 (CLIENT): opens the nearest cremator's door, walks to the firebox with the director, steps in over the lip until the
//     cremator's own begin-overlap fires for it, shuts the door,
//     pulls the lever on its own copy, logs one line a second (health, burn time, the machine's flags, windup, door, actors, health[0], dead, position),
//     pulls again at 40 s; ends at the death plus ten lines, or at 90 s alive.
//   =host (HOST): once the client's puppet has stood in a firebox for two 1 s checks, shuts the door, pulls the
//     lever on the host's copy and logs the machine plus the puppet's life; ends at its death or 120 s.
//   =placeonly (CLIENT, with =host): walks to the firebox and steps in the same way, shuts the door and logs health, burn time, dead.
// Lines: "cremator-probe: DOOR|ROUTE|WALKED|STEPIN|STEPPED|LEVER|T=|placeonly T=|PULLBACK|DIED|SURVIVED|FAIL|DONE", host: "... HOST ...".
// Run: VOTVCOOP_CREMATOR_PROBE=1, --done-marker 'cremator-probe: DONE' (host: 'cremator-probe: HOST DONE').

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::cremator_probe {

// Once per session (the pump's install).
void Install(coop::net::Session* session);

// Game thread, once per pump tick; a single latched read when off.
void Tick();

// Back to the start: a rejoin's peers run the probe again.
void OnDisconnect();

}  // namespace coop::dev::cremator_probe
