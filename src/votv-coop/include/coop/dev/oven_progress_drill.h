// coop/dev/oven_progress_drill.h -- [dev] the oven's screws and lid, raised on one peer's repair
// screen and read on the other's.
//
// Arms (`oven_progress_drill`): host and client -- the acting peer sets a screw's bool on its own
// screen, then the other three, then the lid's, as a screwdriver's turn and the lid's press leave
// them; the watching peer says its screen's mask in the lobby chat whenever it changes, and each
// step must stand there before the next. join -- the host's screen takes all five before anyone
// joins, and the joiner judges that its own screen holds them a moment after its join, from the
// connect replay alone. Every verdict is one line, `[OVEN-DRILL]` with the peer's tag and `PASS` or `FAIL`. An oven the
// save holds repaired has no screen, which fails the run by name.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::dev::oven_progress_drill {

bool IsEnabled();

// The drill's pass on either peer. A single read when off. Game thread.
void Tick(coop::net::Session* session);

// Back to the first step. Net disconnect.
void OnDisconnect();

}  // namespace coop::dev::oven_progress_drill
