// coop/dev/build_identity_drill.h -- [dev] the building identity drill: the nail, wall and poster pieces carry keys the
// prop lane can read, on both peers, across a rejoin (ini build_identity_drill / env VOTVCOOP_BUILD_IDENTITY_DRILL on
// BOTH peers with the same value, the peers' folder VOTVCOOP_CAN_DIR absolute; run with mp.py --rejoin and the
// rejoined client's VOTVCOOP_BUILD_IDENTITY_REJOIN=1 through --rejoin-env). A New Game's world holds one level-placed
// customWall_C and five poster_C. Events only; a FAIL is said the moment it is decided and ends the drill.
//   i1 at each peer's world-ready edge, every live piece of the four roots (nail_C, customWall_C,
//      customWall_unfinished_C, poster_C) reads a key through GetInteractableKey -> FAIL at the first that reads None.
//   i2 the level wall's and the five posters' keys are the same on the host and the client (the client leaves its
//      census as a peer signal; the host compares) -> FAIL at the compare.
//   i3 the client rejoins (a second process): its census holds the same keys -> FAIL at the rejoined compare.
//   i4 the client's load episode ended with no piece counted unsettled -> FAIL at its edge.
// Values: off | green | key_read_off (a piece reads keyless again: i1 FAILs) | settle_count_off (the settle count
// ignores the key read: i4 FAILs). The host ends `rig: VERDICT build-identity PASS`. Off, nothing is registered.
// Game thread.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::build_identity_drill {

// Every pump tick, any role, with or without a session. A latched read when off.
void Tick(coop::net::Session& session);

}  // namespace coop::dev::build_identity_drill
