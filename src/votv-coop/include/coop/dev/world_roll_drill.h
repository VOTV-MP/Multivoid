// coop/dev/world_roll_drill.h -- drill: the day's world rolls, each a host roll that must cross and a client's
// own that must be refused (ini world_roll_drill=off|eye_host|eye_client / env VOTVCOOP_WORLD_ROLL_DRILL; BOTH
// peers, a fresh host world). Every phase waits on a state its peer can read. eye_host: once a client's join is
// over (the slot world-ready, its bracket closed) the host's sky runs its noon setEye(true), and the client's
// copy must show the eye within 20 s; eye_client: the joined client runs its own setEye(true), as its noon roll
// would, and the gate must refuse it, leaving the copy's eye as it was. A refused step ends the arm INVALID.
// The evidence: each arm's DONE line on the client.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::world_roll_drill {

// True unless the row is `off`.
bool IsEnabled();

// Cache the session, for the role. Called from the dev lanes' Install fanout every pump tick; idempotent.
// No-op when off.
void Install(coop::net::Session* session);

// Advance this peer's arm. Every pump tick in a world. Game thread.
void Tick();

// The session ended: back to the first phase.
void OnDisconnect();

}  // namespace coop::dev::world_roll_drill
