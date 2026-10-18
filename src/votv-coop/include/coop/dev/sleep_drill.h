// coop/dev/sleep_drill.h -- drill: what a host's sleep and a client's rest do to the clock and the needs
// (ini sleep_drill=off|host_asleep|client_rest|beside / env VOTVCOOP_SLEEP_DRILL; BOTH peers, a fresh host world,
// a client joined). Every phase waits on a state its peer can read, never on a clock; a phase that does not
// come ends FAIL with the reason after a bound. Each verdict is one line, "[SLEEP-DRILL] <arm> <host|client>
// PASS|FAIL <numbers>", and the host's last line of an arm is its DONE.
//   host_asleep: the host sets its need to 80 and goes to bed through the game's own sleep, and says the
//     world's dilation it reads asleep (20 is the game's). The client, on the host's sleeping flag, reads its
//     own world's dilation (1) and its clock for 10 s; the clock must advance by at least 0.9 times the
//     native rate at 20. The host's DONE comes when it wakes. The host holds its nightmare chance at 0 for
//     the arm (a roll at 15% every 25 s would wake it before the need reaches 100), and puts it back.
#pragma once

// client_rest: the host sleeps as above and prints the real seconds its sleep took (T_host). When the host
// wakes, the client reads its food for 10 s awake, then sets its need to 80 and sleeps in the bed nearest
// it. It reads its world's dilation (1), its food and its need at both ends and prints its real seconds
// (T_client): the two logs are compared, since no wire carries T_host. PASS: the world stayed at 1, the rest
// ended at a need of 99 or more, and the food fell per real second within 25% of the awake rate. The host's
// DONE comes when the client's state is back to awake.
//
// beside: the hook drag selftest (hookdrag_selftest=1, hookdrag_role=client, on both peers) runs beside a
// sleeping host. The host sleeps as above and is HELD asleep, its need written to 50 every tick so the
// game's wake at 100 never comes, until the host's drag has ended (coop/dev/hookdrag_selftest HostEnded:
// printed its verdict, passed or failed); then it writes 100 and the game's own poll wakes it. PASS: the
// world read the game's dilation (20) at every read while held. The host's DONE comes at the wake. A world
// event that wakes it first ends the arm INCONCLUSIVE; any other early wake is a FAIL. The client prints
// its armed line and nothing else; the hook drill is its measure. A host without the hook drill armed
// ends the arm FAIL at its start: nothing would ever end the hold.

namespace coop::net { class Session; }

namespace coop::dev::sleep_drill {

// True unless the row is `off`.
bool IsEnabled();

// Cache the session. Called from the dev install fanout; no-op when off.
void Install(coop::net::Session* session);

// Advance this peer's phase. Every pump tick in a world. Game thread.
void Tick();

// The session ended: back to the first phase, the host's nightmare chance restored.
void OnDisconnect();

}  // namespace coop::dev::sleep_drill
