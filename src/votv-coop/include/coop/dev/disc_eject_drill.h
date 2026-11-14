// coop/dev/disc_eject_drill.h -- [dev] a disc edited in the PC and ejected, read on both peers
// (coop/gamefix/disc_eject_fix).
//
// Arms (`disc_eject_drill`): the host seeds a floppy with three marked rows and 20 writes and
// publishes it. The acting peer (the host; the client for `client`) inserts it into the PC, drops
// its first row and adds one as the file screen's moves do, spends a write, and ejects. The disc
// that comes out must hold the edited rows and 19 writes on the acting peer, and the watching
// peer, which says what its copy of the marked disc holds whenever that changes, must read the
// same. `zip`: the same with a zip drive. `nofix`: the host acts with the fix off, the control
// arm, which must FAIL with the disc as it went in. One `[DISC-EJECT-DRILL]` with the peer's tag and `PASS` or `FAIL`
// line from the acting peer. It leaves the edited disc in the world: run it on a throwaway save.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::dev::disc_eject_drill {

bool IsEnabled();

// The drill's pass on either peer. A single read when off. Game thread.
void Tick(coop::net::Session* session);

// Back to the first step. Net disconnect.
void OnDisconnect();

}  // namespace coop::dev::disc_eject_drill
