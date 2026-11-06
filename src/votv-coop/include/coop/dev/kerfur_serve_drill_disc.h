// coop/dev/kerfur_serve_drill_disc.h -- leg D of the Omega serve drill: a MEASUREMENT of what a client's get_reports
// does when the client holds a disc (env VOTVCOOP_KERFUR_SERVE_DISC with the drill on; BOTH peers). It replaces legs
// A to C. The client walks to the host's Omega, takes a red disc, aims at the Omega's mirror and says what it holds;
// the host snapshots its side (the Omega's hasFloppy, the client's hand on its lane, the disc's key, the discs
// standing) and answers. The client then presses get_reports as a player does: the relayed command, and the radial
// dispatch (setActionIndex 5, useSelectedAction) that runs the verb on its own copy. The host prints
//   [KERFUR-SERVE] leg D: client disc before=<class> after=<gone|held> host insert=<0|1> host still holds the
//   client's disc key=<0|1>
// and says 'rig: VERDICT kerfur-serve-disc PASS' (its own tag, so it does not share kerfur-serve's latch). It never
// fails on what it finds; a step that could not run, or a command the host never ran, is INVALID.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::kerfur_serve_drill::disc {

// Whether leg D is on (the knob, beside the drill's own). Resolved once.
bool Enabled();

// This peer's leg D, every pump tick in a world. Host: a state machine on the game thread. Client: starts one
// worker thread, which waits on the game thread for what it reads. Game thread.
void Tick(coop::net::Session* s);

// The session ended: back to the first phase.
void OnDisconnect();

}  // namespace coop::dev::kerfur_serve_drill::disc
