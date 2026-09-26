// coop/dev/meadow_selftest.h -- [dev] the laptop's signal database lane end to end, through every verb
// that writes the database: the host adds two rows, renames one through the game's rename window, moves
// the other past it through the list's arrows and removes both; then the client adds a row of its own and
// removes it; then a race, a row on each peer that the other has not seen yet; then a row the client adds
// and removes with its lines held, whose two lines must reach the host in their order.
//   HOST   -- once a client's world is ready, it runs its verbs, each once the lane has sent the one
//             before, then watches its own database take the client's row and give it back. That row
//             leaving is the race's cue: it adds row Y, watches the client's X land after it, and takes Y
//             out once X has left, then watches the client's X2 come and go. Its DONE line says the lane
//             sent exactly its verbs' lines and at least one canonical order after the client's lines.
//   CLIENT -- from its own world-ready it watches its database after every line its lane applies: the
//             host's two rows in the host's order, the rename in the renamed row's place, the move, and
//             the database back where it started. Then it adds its own row C; holds its appends
//             (meadow_db_sync::DebugHoldAppends) and adds X, so X is here before anything the host adds
//             next; takes C out, the host's cue; takes the host's Y after X, releases X, and waits for its
//             database to take the host's order, Y before X, which only the host's canonical brings; then
//             adds X2 with its lines held, removes it and releases them. Its DONE line says it sent exactly
//             its own six lines, so nothing it applied went back out.
// Each peer builds the rows alike, so their content hashes are the same on both. "[meadow_selftest]
// FAIL" is the lane failing a step (a run's --fail-marker), "[meadow_selftest] ABANDONED" the drill
// unable to do its part (--dead-marker); the host's DONE line comes last (--done-marker). The rows go
// into the save's real database, and a leg that ends early takes the drill's rows back out, retrying, so a
// row that stays is said loudly; a session that ends mid-leg leaves them, and the next run's arm finds them
// and takes them out. The rename window's exit sets the host's input mode, as a player's rename does. Run
// with meadow_selftest=1.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::meadow_selftest {

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// The per-session half: a rejoin's peers run their legs again, the DONE lines naming the session.
void OnDisconnect();

}  // namespace coop::dev::meadow_selftest
