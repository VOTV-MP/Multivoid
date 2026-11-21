// coop/dev/basedmove_drill.h -- drill: what a client riding the base's cargo lift does to the host's lift (ini basedmove_drill /
// env VOTVCOOP_BASEDMOVE_DRILL on BOTH peers; the ride_nocoll arm is basedmove_puppet_nocoll on the host).
//
// A MEASURE, not a test: its end is `rig: VERDICT basedmove-<ride|ride_nocoll> MEASURED -- <numbers>` (the rig's
// --done-marker; the verdict words of coop/session/rig_verdict.h stay PASS, FAIL and INVALID, and a missing precondition says
// INVALID there). The client is walked onto the lift's platform by the director, never teleported; the host presses the
// lift with the game's own call and, every frame until the lift stops or the Blueprint reverses it, logs the numbers its
// reversal reads and what its engine thinks of the client's puppet. The client logs each frame its own height, its copy of
// the platform and the movement base its engine reads (ue_wrap/actors/character_base).
// Its numbers decide the design of a pose that names the base it stands on.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::basedmove_drill {

// Advance this peer's steps. Every pump tick in a world. Game thread. A latched bool read when off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::basedmove_drill
