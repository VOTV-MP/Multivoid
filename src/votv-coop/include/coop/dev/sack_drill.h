// coop/dev/sack_drill.h -- [dev] a client's point sack is paid by the host, once.
//   HOST   -- once slot 1's world is ready and its puppet stands, spawns a prop_pointSack_C a metre in front of the
//             puppet and reads its own balance and the sack's points; DONE when its balance rose by exactly those
//             points with one paid redemption, FAIL when no pay came within 60 s or the balance moved by another sum.
//   CLIENT -- once its world is ready, finds the sack's mirror, turns its own interaction trace onto it with the aim
//             fan, presses useSelectedAction (the player's E) and says when its mirrored balance rose.
// The red: point_sack_client_runs=1 on the client lets the client's own body run, so the host is never paid and its
// FAIL ends the run. "[SACK-DRILL] FAIL" is the lane failing (--fail-marker), "[SACK-DRILL] ABANDONED" the drill unable
// to do its part (--dead-marker). Run with sack_drill=run and --done-marker "[SACK-DRILL] host DONE".

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::sack_drill {

// Advances this peer's steps; a latched read when off. Game thread.
void Tick(coop::net::Session* s);

// The session ended: the drill starts over.
void OnDisconnect();

}  // namespace coop::dev::sack_drill
