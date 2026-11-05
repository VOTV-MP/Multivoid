// coop/dev/cremator_probe.h -- [dev] what the cremator Blueprint's own burn does to a coop player (MUTATES the world).
// M-b moves the target by the host's TeleportClient order -- the `/cremate` verb under test, not a drill's
// convenience teleport (the rule that a drill moves its peer with the director binds drills; the admin verb's own
// move is what is measured here).
//   =1 (CLIENT): opens the nearest cremator's door, walks to the firebox with the director, steps in until the
//     cremator's own begin-overlap fires for it, shuts the door, pulls the lever on its own copy, logs a line a second.
//   =host (HOST): once slot 1's join has settled, opens the nearest door and sends slot 1 a TeleportClient to the firebox;
//     the landing is the cremator's begin-overlap for the puppet (600 ticks bound). Then shuts the door, pulls the lever.
//   =placeonly (CLIENT, with =host): moves nothing; waits for the host's teleport (over 300 cm from where it stood at
//     join), then logs health, burn time, dead and position each second.
// Lines: "cremator-probe: DOOR|ROUTE|WALKED|STEPIN|STEPPED|LEVER|T=|PULLBACK|DIED|SURVIVED|FAIL|DONE", host:
//   "... HOST DOOR|SENT|LANDED|LEVER|T=|VERDICT PASS|VERDICT FAIL|FAIL|DONE" (PASS: the burn-health <= 0 with the
//   puppet still collected and the same actor for 10 further samples; FAIL: a respawn, or 300 s without that), placeonly: "... placeonly waiting|MOVED|T=".
// Run: VOTVCOOP_CREMATOR_PROBE=1, --done-marker 'cremator-probe: DONE' (host: 'cremator-probe: HOST DONE').

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::cremator_probe {

// Called every pump tick (the dev lanes' retry pump); idempotent.
void Install(coop::net::Session* session);

// Game thread, once per pump tick; a single latched read when off.
void Tick();

// Back to the start: a rejoin's peers run the probe again.
void OnDisconnect();

}  // namespace coop::dev::cremator_probe
