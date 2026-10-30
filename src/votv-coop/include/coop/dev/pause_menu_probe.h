// coop/dev/pause_menu_probe.h -- probe: what the pause menu is made of and what a list of rows costs inside
// it (ini pause_menu_probe=0..3 / env VOTVCOOP_PAUSE_MENU_PROBE). Once per process, when the peer is settled in
// a world, it opens the pause menu through its own enterPause and closes it through resume, three times, and
// prints "pmprobe" lines: m1 the widget tree and whether the third open rebuilt it, m4 frame time (300 frames,
// the tick source named), arm 2 adds m3 (rows built and rebound inside the open menu, the settings row class),
// arm 3 adds m5 (a click and a slide bound to a native function an interceptor swallows, pressed as the engine
// broadcasts them; how enterPause and resume are reached from Escape). With a peer (m2) the client runs the
// arms and says its edges on drill chat: it holds its pause open for a window, and both peers log the wire
// counters, the paused state and the hotkeys' state; the host only watches. It ends with "pmprobe DONE <arm>"
// or "pmprobe FAIL: <why>". Every wait ends on a readiness condition; a bound on it is only a fail-safe.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::pause_menu_probe {

// Advance this peer's phases. Every pump tick in a world; a latched read when off. Game thread.
void Tick(coop::net::Session* session);

// The session ended: a pending wait on the other peer is dropped. The once-per-process latch is not cleared.
void OnDisconnect();

}  // namespace coop::dev::pause_menu_probe
