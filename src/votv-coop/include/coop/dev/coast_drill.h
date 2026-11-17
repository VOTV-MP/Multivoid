// coop/dev/coast_drill.h -- drill: a crate a hand lets go of comes to rest alike on both peers
// (ini coast_drill=off|letgo|unelected|join / env VOTVCOOP_COAST_DRILL; BOTH peers).
//
// The host spawns a crate (prop_box_C) on measured open ground before the client's puppet. The acting peer walks to it by the
// bot director, takes it by the aimed grab the use key runs, and lets go with a plain release. Both peers wait for their copy
// to rest; the client says its rest pose and whether the host's drive End reached its copy, and the host compares them
// (coop/dev/rest_compare). letgo: the host acts. unelected: the client acts, and the host, which elects no syncer for a release
// with no throw, flies it. join: the host holds the crate until a joiner is loading, lets go, and the crate rests before the
// joiner's world is up; the joiner's snapshot carries the rest. The client's life 1 prints `[COAST-DRILL] [C] REJOIN-NOW`
// (run with the rig's rejoin and VOTVCOOP_COAST_LIFE=2 for the second life).
//   rig: VERDICT coast-letgo | coast-unelected | coast-join  PASS | FAIL | INVALID
// FAIL: a gap past 1 cm and 1 degree; within it, no let-go coast having flown the crate, or no End at the client's copy.
// Red: coast_letgo_off with body_push_off (letgo, unelected) FAILs at the gap. The join arm has no red: its snapshot reads the
// host's pose when it is expressed, whatever flew the crate, so no lever of the let-go's can fail it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::coast_drill {

// True unless the row is `off`.
bool IsEnabled();

// Advance this peer's steps. Every pump tick in a world. Game thread.
void Tick(coop::net::Session* session);

// The session ended: back to the first step.
void OnDisconnect();

}  // namespace coop::dev::coast_drill
