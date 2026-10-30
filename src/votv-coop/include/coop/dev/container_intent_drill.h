// coop/dev/container_intent_drill.h -- [dev] the container intents drill: a client takes from a shared world
// container by the player's own click, and both peers print what they saw.
//   BOTH   -- pick the same container: the smallest element id, within 5000 cm of the player, that holds two records
//             or more, is not the player's own inventory and is reached by a nav route; the race arms (race, two,
//             sale, room) take one whose first record is unique before any other, and with none reached the nearest
//             qualifying one is the target; re-picked once a second.
//   CLIENT -- walks to it with the director, opens it through the game's own openPropInv, presses the screen's
//             first slot as a click does, waits for the take's commit, and prints the counts and the verdict.
//   HOST   -- prints the container's count and the act holds each time they change, and judges the count's timeline.
// The arms are the values of the container_intent_drill config row, each described there. Reds: race, two and sale
// container_act_no_escrow=1; forge container_slice_accept_red=1; the put arms their own container_act_* rows. The
// throw arms (container_intent_drill_throw*.cpp) need container_route_census=1; throwinto lands a thrown drive, a dropped one,
// a held one and a taken-from basket's in a shared container (n=1..5; n=5 needs a second client). Lines are tagged [CI-DRILL]; each peer prints `<role> <arm> PASS|FAIL|MEASURED`
// (throwinto: its host prints `-> PASS|FAIL: <why>` per sub-run).

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::container_intent_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off. A walk thread of its own on the client.
void Tick(coop::net::Session* session);

// The target, the screen and the walk belong to one world and one session.
void OnDisconnect();

}  // namespace coop::dev::container_intent_drill
