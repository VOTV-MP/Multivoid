// coop/dev/midnight_drill.h -- drill: a midnight where the day rollover can be watched (ini
// midnight_drill=off|awake|asleep|cheat|mode5 / env VOTVCOOP_MIDNIGHT_DRILL; BOTH peers, with
// rollover_watch on, on a fresh host world, since a save can carry an active event).
// Every phase waits on a state its peer can read, never on a clock. The host arms once a client's join
// is over by its own account (the slot world-ready, its bracket closed). awake sets the clock to 0.999 of
// the day; asleep sets 0.98 while awake (a forward jump can start an event, which refuses sleep), waits
// for no event, then goes to bed, as the client does once joined; both print their dilation and time
// scale at the shared fast-forward, and the host its runway at every set. Before the midnight each peer
// clears its music flags, so the rollover's setting them again shows. cheat: the host leaves its clock
// alone, and the joined client writes a day onto its own three times, as the cheat menu's day button
// does, and says how each next tick ended (its own midnight rolled, the lane held its last sample over
// it, or a new one met it). mode5 spawns game mode 5's master on each peer once joined; the client says
// whether its own reset kept running. A refused step ends the arm INVALID. The evidence: rollover_watch's
// DAY and OUTPUTS lines for awake and asleep; for cheat and mode5, its run counts and the lane's count.
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
