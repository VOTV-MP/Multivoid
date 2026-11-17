// coop/dev/cheat_drill.h -- drill: the host's and the players' cheat tools, end to end on two peers (ini cheat_drill=
// off|on / env VOTVCOOP_CHEAT_DRILL; BOTH peers, one run), sequenced on lobby-chat lines (coop/dev/drill_chat). Each peer
// opens the F1 panes it is offered, then: A the host spawns crowbars through Items and the client gains them; B points
// through Economy mirror; C an event through Events is replayed; D with the setting off the client's three requests are
// answered Off; E with it on the client's points, crowbars, spawn at the host and event run and a story event and an
// unlisted prop are refused; G..L the hostile arms (cheat_drill_arms): raw requests no panel makes, each refused with its
// reason, a ten-prop request draining the bucket, eight removals against 50000, two requests in flight, a withheld right,
// Unstuck onto the NavMesh; F the host resets the setting and the next request is Off. A FAIL is decided by an event and
// said to the peer at once; a time bound only says INVALID. Reds, [dev] rows on the HOST (the last on both peers):
// cheat_node_check_off (arm K must FAIL), cheat_open_check_off (E's story event must FAIL), cheat_count_cost_off (H must
// FAIL). Lines tagged [CHEAT-DRILL]; the host's line is the verdict. Nothing here ships enabled.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::cheat_drill {

// Every pump tick in a world; a latched read when off. Game thread.
void Tick(coop::net::Session* session);

// The session ended: the drill starts over, a menu it opened is closed, its observer let go.
void OnDisconnect();

}  // namespace coop::dev::cheat_drill
