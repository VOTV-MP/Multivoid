
// coop/dev/container_intent_drill.h -- [dev] the container intents drill: a client takes from a shared world
// container by the player's own click, and both peers print what they saw.
//   BOTH   -- pick the same container: the smallest element id, within 5000 cm of the player, that holds two records
//             or more, is not the player's own inventory and is reached by a nav route; the race arms (race, two,
//             sale, room) take one whose first record is unique before any other, and with none reached the nearest
//             qualifying one is the target; re-picked once a second.
//   CLIENT -- walks to it with the director, opens it through the game's own openPropInv, presses the screen's
//             first slot as a click does, waits for the take's commit, and prints the counts and the verdict.
//   HOST   -- prints the container's count and the act holds each time they change, and judges the count's timeline.
// The arms are the values of the container_intent_drill config row (take, settle, abort, deadline, double, held,
// ghost, race, two, sale, depart, room, putrecord, puthand, putfood, puthidden, putnested, putempty), each described
// there. The red of race, two and sale is container_act_no_escrow=1. The put arms pick no world container: the host
// spawns the target and the item (container_intent_drill_put*.cpp). Lines are tagged [CI-DRILL]; each peer prints
// `<role> <arm> PASS|FAIL`.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::container_intent_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off. A walk thread of its own on the client.
void Tick(coop::net::Session* session);

// The target, the screen and the walk belong to one world and one session.
void OnDisconnect();

}  // namespace coop::dev::container_intent_drill
