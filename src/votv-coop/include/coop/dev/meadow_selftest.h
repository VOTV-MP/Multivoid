// coop/dev/meadow_selftest.h -- [dev] the laptop's signal database lane end to end, through every verb
// that writes the database: the host adds two rows, renames one through the game's rename window, moves
// the other past it through the list's arrows and removes both; then the client adds a row of its own and
// removes it.
//   HOST   -- once a client's world is ready and its lane holds this world's database, it runs its verbs,
//             each once the lane has sent the one before, then watches its own database take the client's
//             row and give it back. Its DONE line says the lane sent exactly its verbs' lines.
//   CLIENT -- from its own world-ready it watches its database after every line its lane applies: the
//             host's two rows in the host's order, the rename in the renamed row's place, the move, and
//             the database back where it started. Then it adds and removes its own row. Its DONE line
//             says it sent exactly its own two lines, so nothing it applied went back out.
// Each peer builds the rows alike, so their content hashes are the same on both. "[meadow_selftest]
// FAIL" is the lane failing a step (a run's --fail-marker), "[meadow_selftest] ABANDONED" the drill
// unable to do its part (--dead-marker); the host's DONE line comes last (--done-marker). The rows go
// into the save's real database, and every leg that ends early takes its rows back out, retrying, so a
// row that stays is said loudly. The rename window's exit sets the host's input mode, as a player's
// rename does. Run with meadow_selftest=1.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::meadow_selftest {

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// The per-session half: a rejoin's peers run their legs again, the DONE lines naming the session.
void OnDisconnect();

}  // namespace coop::dev::meadow_selftest
