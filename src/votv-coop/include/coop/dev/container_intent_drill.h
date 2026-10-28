// coop/dev/container_intent_drill.h -- [dev] the container intents drill: a client takes from a shared world
// container by the player's own click, and both peers print what they saw.
//   BOTH   -- pick the same container: the smallest element id that holds two records or more and is not the
//             player's own inventory, re-picked once a second until found.
//   CLIENT -- walks to it with the director, opens it through the game's own openPropInv, presses the screen's
//             first slot as a click does, and waits for the take's commit; one second after it, prints the
//             container's and the player's own counts, the time from the press to the commit, and the verdict.
//   HOST   -- prints the container's count each time it changes and the act holds with the host's tallies each
//             time they change; passes when the count fell by exactly one and no hold stood three seconds later.
// Lines are tagged [CI-DRILL]. Run with container_intent_drill=take; the host's DONE line ends it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::container_intent_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off. A walk thread of its own on the client.
void Tick(coop::net::Session* session);

// The target, the screen and the walk belong to one world and one session.
void OnDisconnect();

}  // namespace coop::dev::container_intent_drill
