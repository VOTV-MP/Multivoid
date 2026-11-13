// coop/dev/pause_quit_drill.h -- drill: a peer quits to the main menu through the pause menu's own
// button (ini pause_quit_drill=off|client|host / env VOTVCOOP_PAUSE_QUIT_DRILL; the named role only).
//
// Once per process, when the peer is settled in the session, it sends the player's Escape event,
// waits until the pause menu is open, and calls the click handler of its "Main menu" button, as a
// player's click does. The click runs lib_C::loadLevel("menu") through the game's own script, so
// the quit decision (coop/player/run_end_travel) is the real one. The press is judged by whether it
// reached the menu travel: `rig: VERDICT pause-quit PASS|FAIL|INVALID`. The rig also reads the
// quitter's log (the session's state at the menu world's destroys) and the other peer's. With the
// pause_quit_event row the peer first starts an event (lib_C::setEvent) and waits until the game counts
// it, so the press meets a running event, which coop/player/leave_in_event answers (its red is the row
// leave_in_event_red). Lines tagged [PAUSE-QUIT]: "<role>: event started", "<role>: Escape sent",
// "<role>: Main menu pressed", "<role> FAIL: <why>" (the click reached no travel) and
// "<role> ABORT: <why>" (the case could not be exercised: a wait ran out, the menu was not found).

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::pause_quit_drill {

// Advance this peer's phases. Every pump tick in a world; a latched read when off. Game thread.
void Tick(coop::net::Session* session);

// The session ended: a pending wait for the pause menu is dropped. The once-per-process latch is
// not cleared.
void OnDisconnect();

}  // namespace coop::dev::pause_quit_drill
