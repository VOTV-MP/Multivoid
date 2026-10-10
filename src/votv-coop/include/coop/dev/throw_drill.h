// coop/dev/throw_drill.h -- [dev] a container a client throws keeps its contents on the host (L7).
//   HOST   -- once slot 1's world is ready and its puppet stands, spawns a backpack a metre before the puppet, puts two
//             drives into it with the game's own addObject and takes the contents' digest; then finds its copy of
//             the backpack the client's throw births. run: DONE when the copy's digest equals the fixture's. red
//             (the client skips its slice): FAIL when the copy is still empty past the transfer's 30 s, naming
//             whether the transfer expired. gone: once the copy awaits slot 1's slice, kicks slot 1 and is DONE when
//             the transfer ended with its author.
//   CLIENT -- once its world is ready, finds the backpack's mirror holding the host's two records, waits for it to
//             lie still, takes it into the hand (Hold Object) and throws it (throwHoldingProp), the game's own birth.
// red and gone run with container_birth_skip_slice=1 on the client, and gone with the rig's --observer: a second
// client keeps the host's session alive, so the kick ends the transfer through the peer-gone path rather than with
// the whole session (with one client the kick is the session's end). "[THROW-DRILL] FAIL" is the lane failing
// (--fail-marker), "[THROW-DRILL] ABANDONED" the drill unable to do its part (--dead-marker). Run with
// throw_drill=run|red|gone and --done-marker "[THROW-DRILL] host DONE".

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::throw_drill {

// Advances this peer's steps; a latched read when off. Game thread.
void Tick(coop::net::Session* s);

// The session ended: the drill starts over.
void OnDisconnect();

}  // namespace coop::dev::throw_drill
