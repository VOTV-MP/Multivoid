// coop/dev/keypad_drill.h -- the keypad lane's drill: a client's input reaches the host, the host
// judges it, and the client's copy lands on the host's verdict. CLIENT -- once its join is over,
// walks with the director to the keypad its navmesh reaches first among the named ones that gate a
// door and have a code of digits, and runs five legs there, each started in one tick and ended by its
// own copy's state: PRESS the door while the keypad is unlocked (first when it starts so): its open
// flips; ACCEPT the code on the numpad: active 1, the buffer empty, the door's 1; CANCEL two numpad
// digits, once shown, with "-": active 0; DENY a wrong code on the keys: active 0, the door's 0; TAIL,
// the same and two digits more in that tick, held through the deny's 0.2 s tail: the buffer ends as
// those two. Straight after each start its own copy reads as before. HOST -- logs every change of
// every keypad that gates a door. LATE: once slot 1's world is taken for its join, before it is
// ready, the host negates as its own chain would the verdict of the first named keypad (by key) that
// gates a door with a code the keys cannot type; the client, at its end, reads that door as its
// keypad, which only the snapshot carried. Both census the keypads that gate a door, door beside
// keypad: host at start and after the flip, client at start and end. A leg not landed in 10 s fails.
// Lines are tagged [KEYPAD-DRILL]; run on both peers (keypad_drill=1); the client's DONE ends it.

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
