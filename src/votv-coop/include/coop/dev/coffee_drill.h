// coop/dev/coffee_drill.h -- [dev] a press of a coffee device on the client, and the maker's state, are the host's.
// Set coffee_drill=1 on BOTH peers, on a fresh host world. The host spawns a maker by a free powered wall socket,
// plugs it in, and names the devices in the lobby chat; the client's player walks by the director and presses them
// through the game's own action verb. Five arms, each closing when both peers print what they see:
//   1 the client presses the vending machine: the host's points drop by 15 once, a cup is live on both
//   2 the host drops an empty cup and a bag onto the maker: flags 1,1 on both, the client powered, cup gone
//   3 the client presses the maker: a cup is live on both, both flags read 0
//   4 the client lets its OWN copy of an empty cup fall on its copy of the maker: the hit is cancelled and counted,
//     the maker stays empty and the cup lives on both; no such hit leaves the arm INCONCLUSIVE (arm 5 likewise)
//   5 the client carries an empty cup over the maker: hasCup=1 on both, the cup gone on both
// The client prints `[COFFEE] client arms 1-5 PASS` (the rig's rejoin marker); the rejoined client reads the maker
// as the host holds it and prints `[COFFEE] client DONE -- PASS`. A line `[COFFEE] FAIL:` is an arm not closed within
// its bound (it starts when the client has arrived), a walk that did not arrive, or a step that cannot be driven.
// coffee_red (no_intent | no_publish | no_cancel) turns one gate of the lane off, for the red arm.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::coffee_drill {

// Store the session; idempotent, called at install. A latched read when off.
void Install(coop::net::Session* s);

// Advances this peer's steps; a latched read when off. Game thread.
void Tick();

// The session ended: the client starts over; the host starts over too, unless its five arms are done, when it keeps
// its cast for the rejoin.
void OnDisconnect();

}  // namespace coop::dev::coffee_drill
