// coop/dev/meadow_selftest.h -- [dev] the laptop's signal database lane end to end: the host adds a row
// to its database and takes it out again, and the client's copy of the database follows.
//   HOST   -- once a client's world is ready and its own lane holds this world's database, it adds the
//             row through the laptop's addSignal, waits for the lane to send it, removes it through
//             removeSignal and waits for the lane to send the removal. Its DONE line says both went.
//   CLIENT -- from its own world-ready it watches its database: the host's row arrives, then leaves, and
//             the database ends at the count it started from. Its DONE line is the verdict.
// The row is the same on both peers (named MEADOW-SELFTEST, id selftest-0), so each finds it by the
// lane's own content hash. "[meadow_selftest] FAIL" is the lane failing a step (a run's --fail-marker),
// "[meadow_selftest] ABANDONED" the drill unable to do its part (--dead-marker). It writes a row into
// the save's real database and retries the removal, so a removal that never lands is said loudly.
// Run with meadow_selftest=1.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::meadow_selftest {

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// The per-session half: a rejoin's peers run their legs again, the DONE lines naming the session.
void OnDisconnect();

}  // namespace coop::dev::meadow_selftest
