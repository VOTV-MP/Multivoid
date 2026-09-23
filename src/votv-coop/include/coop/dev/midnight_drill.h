// coop/dev/midnight_drill.h -- drill: bring the host's midnight to where the day rollover can be
// watched (ini midnight_drill=off|awake|asleep / env VOTVCOOP_MIDNIGHT_DRILL; BOTH peers, with
// rollover_watch on, on a fresh host world, since a save can carry an active event).
//
// Every phase waits on a state its peer can read, never on a clock. The host arms once a client's
// join is over by its own account -- the slot world-ready and its bracket closed. awake sets the
// clock to 0.999 of the day. asleep sets 0.98 while still awake (a forward jump can start an event,
// and an active event refuses sleep), waits for no event to be active, then goes to bed; the client
// goes to bed once it has joined. Both print their dilation and time scale when the shared
// fast-forward begins, and the host prints the runway at every set, so an arm's premise is in the
// log. A refused step ends the arm INVALID with its reason. The evidence is rollover_watch's: the
// client's DAY line keyed on the host's day number.

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
