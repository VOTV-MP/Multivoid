// coop/dev/oven_progress_drill.h -- [dev] the oven's screws and lid, raised on one peer's repair
// screen and read on the other's.
//
// Arms (`oven_progress_drill`): host and client -- the acting peer sets a screw's bool on its own
// screen, then the other three, then the lid's, as a screwdriver's turn and the lid's press leave
// them; the watching peer says its screen's mask in the lobby chat whenever it changes, and each
// step must stand there before the next. join -- the host's screen takes all five before anyone
// joins, and the joiner judges its own screen when the host's mask reaches it, from the connect
// replay alone. Every verdict is one line, `[OVEN-DRILL]` with the peer's tag and `PASS`, `FAIL` or
// `INVALID`, and each PASS and FAIL is decided on an arrival: the watching peer's next mask for the
// acting peer, a mask counted at the receiving screen for the watching peer and the joiner. A wait
// whose event never comes ends INVALID after a bound, never FAIL. An oven the save holds repaired has
// no screen, which ends the arm INVALID by name. Red arm: `oven_merge_off` on the watching peer (or the
// joiner) counts the arrival and does not raise the bits, so that peer's judge FAILs at the arrival.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::dev::oven_progress_drill {

// True when any arm of the oven drill runs on this peer. The client of the client arm is walked to the oven's switch
// box by the director before its first step: the host judges a client's steps by their reach of it. The arms repair,
// reach, order and fixearly are coop/dev/oven_judge_drill's.
bool IsEnabled();

// Whether this peer's join is over: the client's world announced ready and its join idle, or a client of the host's
// world-ready with its snapshot bracket closed. Both drills of the oven start on it. Game thread.
bool PeersJoined(coop::net::Session& session);

// The drill's pass on either peer. A single read when off. Game thread.
void Tick(coop::net::Session* session);

// Back to the first step. Net disconnect.
void OnDisconnect();

}  // namespace coop::dev::oven_progress_drill
