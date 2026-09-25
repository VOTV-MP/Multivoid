// coop/dev/keypad_drill.h -- the keypad lane's drill: a client's typing reaches the host, the host
// judges it, and the client's copy lands on the host's verdict.
//   CLIENT -- once its join is over, walks with the director to the keypad its navmesh reaches first
//             among those the lane names that gate a door, and runs three legs there, each typed in
//             one tick and ended by its own copy's state: ACCEPT the password through the numpad
//             (its digits and "+"; a five-digit one submits itself): active 1, the buffer empty and
//             the gated door's active 1; CANCEL two numpad digits, once they show, with its "-":
//             active 0, the buffer empty; DENY a wrong code on the keys and the accept key: active
//             0, the buffer empty, the door's active 0. A keypad already unlocked starts with DENY.
//             Straight after typing, its own copy must read as before: the input runs on the host.
//   HOST   -- logs every change of every keypad that gates a door.
// A leg that has not landed in 10 s fails. Lines are tagged [KEYPAD-DRILL]. Run on both peers of a
// pair (keypad_drill=1); the client's DONE line ends it.

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
