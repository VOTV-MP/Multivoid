// coop/dev/coast_drill.h -- drill: a crate a hand lets go of comes to rest alike on both peers
// (ini coast_drill=off|letgo|unelected|join / env VOTVCOOP_COAST_DRILL; BOTH peers).
//
// The host spawns a crate (prop_box_C) on measured open ground before the client's puppet. The acting peer walks to it by the
// bot director, takes it by the aimed grab the use key runs, and lets go with a plain release. Both peers wait for their copy
// to rest; the client says its rest pose and whether the host's drive End reached its copy, and the host compares them
// (coop/dev/rest_compare). letgo: the host acts. unelected: the client acts, and the host, which elects no syncer for a release
// with no throw, flies it. join: the host holds the crate until a joiner is loading, lets go, and the crate rests before the
// joiner's world is up; the joiner's snapshot carries the rest. Life 1 prints `[COAST-DRILL] [C] REJOIN-NOW` (VOTVCOOP_COAST_LIFE=2: life 2).
//   rig: VERDICT coast-letgo | coast-unelected | coast-join  PASS | FAIL | INVALID
// FAIL: a gap past 1 cm and 1 degree; within it, no let-go coast having flown the crate, or no End at the client's copy.
// Red: coast_letgo_off with body_push_off (letgo, unelected) FAILs at the gap; the join arm has no red (its snapshot reads the
// host's pose, whatever flew the crate). letgo with letgo_release_delay_ms (coop/dev/letgo_release_delay.h): the client holds
// the host's release so the let-go row's first pose meets the hand; INVALID when no yield was kept pending on that release,
// PASS at the host's End pose, and with letgo_unyield_off too FAIL at the same gap.

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
