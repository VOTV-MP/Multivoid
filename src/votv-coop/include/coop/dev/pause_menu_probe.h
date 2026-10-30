// coop/dev/pause_menu_probe.h -- probe: what the pause menu is made of and what a list of rows costs inside
// it (ini pause_menu_probe=0..2 / env VOTVCOOP_PAUSE_MENU_PROBE).
//
// Once per process, when the peer is settled in a world, it opens the pause menu through the menu's own
// enterPause and closes it through resume, three times, and prints one line per measurement, all tagged
// "pmprobe":
//   m4 closed / open / rows   frame time p50 / p99 / max over 300 frames, the tick source named in the line
//   m1 tree                   the live pause menu to depth 4, and whether the third open rebuilt the first's widgets
//   m2 ...                    two peers: the client's pause for a window, the wire counters and the hotkeys on
//                             both peers (the client runs the arms and says its edges on drill chat; the host,
//                             which has a settled peer when it is ready, only watches and logs)
//   m3 ...                    arm 2: rows built inside the open menu, rebound, and the settings row class
// then "pmprobe DONE <arm>", or "pmprobe FAIL: <why>" where a phase cannot be exercised. Every wait ends on a
// readiness condition; a bound on it is only a fail-safe. Arms: 0 off, 1 the tree and the frame time, 2 adds
// the rows.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::pause_menu_probe {

// Advance this peer's phases. Every pump tick in a world; a latched read when off. Game thread.
void Tick(coop::net::Session* session);

// The session ended: a pending wait on the other peer is dropped. The once-per-process latch is not cleared.
void OnDisconnect();

}  // namespace coop::dev::pause_menu_probe
