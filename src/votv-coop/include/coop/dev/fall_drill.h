// coop/dev/fall_drill.h -- [dev] a host birth that falls lands on the client where it lands on the host (L11).
//   HOST   -- once slot 1's world is ready and its puppet stands, spawns a drive 4 m up in front of its own player and
//             says, once the drive is tracked and lies still, where it came to rest and how far it fell.
//   CLIENT -- once its world is ready, waits for the first stream end the host applies here after that, and passes
//             when its copy of that prop rests within 5 cm of the host's final pose the end carried.
// The red: prop_fall_no_coast=1 on the host streams no fall, so no end comes and the client fails. "[FALL-DRILL]
// FAIL" is the lane failing (--fail-marker), "[FALL-DRILL] ABANDONED" the drill unable to do its part (--dead-marker).
// Run with fall_drill=run and --done-marker "[FALL-DRILL] client DONE".

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::fall_drill {

// Advances this peer's steps; a latched read when off. Game thread.
void Tick(coop::net::Session* s);

// The session ended: the drill starts over.
void OnDisconnect();

}  // namespace coop::dev::fall_drill
