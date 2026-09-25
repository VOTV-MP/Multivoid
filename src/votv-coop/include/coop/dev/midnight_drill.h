// coop/dev/midnight_drill.h -- drill: a midnight where the day rollover can be watched (ini
// midnight_drill=off|awake|asleep|cheat / env VOTVCOOP_MIDNIGHT_DRILL; BOTH peers, with
// rollover_watch on, on a fresh host world, since a save can carry an active event).
//
// Every phase waits on a state its peer can read, never on a clock. The host arms once a client's
// join is over by its own account -- the slot world-ready and its bracket closed. awake sets the
// clock to 0.999 of the day. asleep sets 0.98 while still awake (a forward jump can start an event,
// and an active event refuses sleep), waits for no event to be active, then goes to bed, as the
// client does once joined; both print their dilation and time scale at the shared fast-forward, and
// the host its runway at every set. cheat leaves the host's clock alone: the joined client writes a
// day onto its own as the cheat menu's day button does, three times, and says how each next tick
// ended (its own midnight rolled, the clock lane held its last sample over the write, or a new one
// met it). A refused step ends the arm INVALID with its reason. The evidence is rollover_watch's DAY
// lines for awake and asleep; cheat reads its run counts and the clock lane's count of the writes.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::midnight_drill {

// True unless the row is `off`.
bool IsEnabled();

// Cache the session, for the role. Called from the subsystems Install fanout every pump tick;
// idempotent. No-op when off.
void Install(coop::net::Session* session);

// Advance this peer's phase. Every pump tick in a world. Game thread.
void Tick();

// The session ended: back to the first phase.
void OnDisconnect();

}  // namespace coop::dev::midnight_drill
