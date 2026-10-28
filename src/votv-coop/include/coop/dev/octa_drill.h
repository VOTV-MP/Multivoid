// coop/dev/octa_drill.h -- [dev] the Geom Octa drill (octa_drill = births | measure | attack | join), run on all peers.
//   births   -- the host calls the gamemode's own spawn verb with the spawn menu's Octa row; its catch must enrol the Octa
//               (`[OCTA-DRILL] births PASS role=host`) and the client materialise its mirror (`births PASS role=client`);
//               the client's call of the night roll's event is refused (a log line). Red: octa_red=no_source.
//   measure  -- the walker takes its place (the client's director walks it 2200 cm from the host), then the host spawns the
//               Octa 600 cm from it and 1300 cm or more from the others, so the sphere's register-time overlap picks it.
//               It stands for three volleys and walks out 1800 cm, else 2400, else 3000. Liveness only: the host prints
//               `[OCTA-DRILL] measure DONE` at the first idle after three verdicts. Red: octa_red=no_parking.
//   attack   -- measure's placement judged on the relay: the client applies three host verdicts and prints `[OCTA-DRILL]
//               attack DONE`, as the host does. Red: octa_red=no_verdict. octa_spawn_route=gamemode: the gamemode's verb.

#pragma once

//   join     -- THREE peers, run with the rig's --observer: the host walks 2200 cm from client 1 (a witness that stands
//               where it spawned) and spawns the Octa 600 cm from itself, timed off client 1's load time, so the Octa
//               aggros the host about 8 s before the observer (the joiner) is world-ready. The observer's snapshot carries
//               the Octa and the join Aggro: it replays it, applies one verdict, prints `join DONE role=joiner` and says so
//               in chat; the host leaves on that and prints `join DONE`, after `join snapshot: mid-attack` (a snapshot
//               between attacks ends the arm `FAIL: INCONCLUSIVE`). Red: octa_red=no_join (`FAIL: no aggro at join`).
// Every player holds `startInvinc` for the arm's span, and the firing arms demand `damage_probe=1` so each blast logs [DMG].

namespace coop::net { class Session; }

namespace coop::dev::octa_drill {

// Game thread, once per pump tick; a latched string compare when off.
void Tick(coop::net::Session* session);

// The per-session half: the arm's state back to its start, the guard put back.
void OnDisconnect();

}  // namespace coop::dev::octa_drill
