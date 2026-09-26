// coop/dev/meadow_selftest.h -- [dev] the laptop's signal database lane end to end, through every verb that
// writes the database: the host adds two rows, renames one through the game's rename window, moves the
// other past it through the list's arrows and removes both; the client adds a row of its own and removes
// it; then a race, a row on each peer that the other has not seen yet (the client's X, held, and the
// host's Y), which only the host's canonical order settles, Y before X; then X2, which the client adds and
// removes with its lines held, whose two lines must reach the host in their order.
//   HOST   -- runs each verb once the lane has sent the one before, then watches its database take and give
//             back the client's rows. Its DONE line says the lane sent exactly its verbs' lines and at
//             least one canonical order after the client's lines.
//   CLIENT -- checks its database after every line its lane applies. Its DONE line says it sent exactly its
//             own six lines, so nothing it applied went back out.
// "[meadow_selftest] FAIL" is the lane failing a step (--fail-marker), "[meadow_selftest] ABANDONED" the
// drill unable to do its part (--dead-marker), the host's DONE line the --done-marker. The rows go into
// the save's real database; a leg that ends early takes them back out, and the next run's arm takes out
// any a session left. Run with meadow_selftest=1.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::meadow_selftest {

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// The per-session half: a rejoin's peers run their legs again, the DONE lines naming the session.
void OnDisconnect();

}  // namespace coop::dev::meadow_selftest
