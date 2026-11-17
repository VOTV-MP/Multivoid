// coop/dev/body_push_drill.h -- drill: a body walks into a prop, and both peers' copies come to rest alike
// (ini body_push_drill=true / env VOTVCOOP_BODY_PUSH_DRILL; BOTH peers; default off).
//
// The host spawns a woodchipper on measured open ground, taller than the step the client's character climbs. The client's
// director walks it into the woodchipper five times, then the host's own player does once. At each push's rest each peer marks
// its copy's rest pose (the client in a chat line) and the host compares them (coop/dev/rest_compare), within 1 cm and 1
// degree, and says which peer held the push's drive row: the pusher's (a client's row for a client push, the host's for its own).
//   rig: VERDICT body-push PASS     every leg within the bound with its pusher's row, and at least three client pushes with
//                                   contact (within 5 cm) moved a copy by over 2 cm
//   rig: VERDICT body-push FAIL     a leg rested past the bound or had the wrong row: said at that leg's rest, no later leg
//   rig: VERDICT body-push INVALID  a step could not run, or too few pushes made contact and moved a copy
// Reds, each its own run: body_push_claim_off (client) sends no claim, so a push's row is the host's coast, which fails that
// push on the row owner; body_push_off (host) streams no push, which fails the host's own leg (a client push that fails first
// says INVALID). The pushed prop's hit events are counted per push in HITS lines, a probe that no verdict reads.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::body_push_drill {

// True when the row is set.
bool IsEnabled();

// Advance this peer's steps. Every pump tick in a world. Game thread.
void Tick(coop::net::Session* session);

// The session ended: back to the first step.
void OnDisconnect();

}  // namespace coop::dev::body_push_drill
