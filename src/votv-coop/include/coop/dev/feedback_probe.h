// coop/dev/feedback_probe.h -- [dev] where a verb's player-facing feedback plays: every hint and 2D sound a peer makes.
//
// A game verb run for another player (the host's Omega serving a client, a desk press replayed) shows its hint
// (lib_C::addHint) and plays its 2D sounds (GameplayStatics::PlaySound2D) on the machine that runs the body. This
// probe logs each one per peer, so a drill's host and client logs say who heard what. A script-gate watch on addHint
// and a pre hook on PlaySound2D, both only logging and letting the call run. One line per call:
//   [FEEDBACK-PROBE] role=<host|client|solo> hint "<text>" from=<the outermost script function>
//   [FEEDBACK-PROBE] role=<host|client|solo> sound2d "<sound asset>" from=<the calling script function>
// At most 20 lines a second; a second that dropped some says how many. Ini feedback_probe=1 or
// VOTVCOOP_FEEDBACK_PROBE=1, on every peer to be read. Off, nothing is registered. Game thread.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::feedback_probe {

// Cache the session, for the role; register the watch and the hook while the row is on, retried each pump tick until
// each settles. A latched read when off.
void Install(coop::net::Session* session);

// Session end: the dropped count of the window in progress is said.
void OnDisconnect();

}  // namespace coop::dev::feedback_probe
