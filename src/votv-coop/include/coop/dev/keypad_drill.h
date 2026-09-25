// coop/dev/keypad_drill.h -- the keypad lane's drill: a client's input reaches the host, the host
// judges it, and the client's copy lands on the host's verdict.
//   CLIENT -- once its join is over, walks with the director to the keypad its navmesh reaches first
//             among those the lane names that gate a door, and runs five legs there, each started in
//             one tick and ended by its own copy's state. PRESS the gated door while the keypad is
//             unlocked, first when it starts so (a joiner's door, no keypad input): its open flips.
//             ACCEPT the password on the numpad: active 1, the buffer empty, the door's active 1.
//             CANCEL two numpad digits, once they show, with its "-": active 0. DENY a wrong code on
//             the keys and the accept key: active 0, the door's 0. TAIL, last, the same and two more
//             digits in the same tick, which the host holds through the deny's 0.2 s tail: the buffer
//             ends as those two. Straight after each start, its own copy reads as before.
//   HOST   -- logs every change of every keypad that gates a door, with its door's active.
// Each censuses the named keypads that gate a door (a pair's two share one), the door's active beside
// the keypad's: the host at its start, the client at its start and its end. A leg not landed in 10 s
// fails. Lines are tagged [KEYPAD-DRILL]; run on both peers (keypad_drill=1); the client's DONE ends it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::keypad_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off. A walk thread of its own, then one
// keypad read a tick on the client; the host reads the keypads that gate a door.
void Tick(coop::net::Session* session);

// The keypad and the legs belong to one world and one session.
void OnDisconnect();

}  // namespace coop::dev::keypad_drill
