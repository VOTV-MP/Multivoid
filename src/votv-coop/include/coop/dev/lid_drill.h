// coop/dev/lid_drill.h -- [dev] the portable PC's lid across both peers. The rig's save has no portable
// PC, so the host brings one in.
//   HOST   -- once a client's world is ready: loads the PC's class, spawns one in front of its player,
//             and once the element lane names it opens the lid with the PC's own action 10, as a
//             player's use does. Once the client's close reaches its copy -- its lane having sent one
//             line and applied one -- it opens the lid again and holds it for the next joiner, whose copy
//             loads closed, so only the joiner's world-ready row can open it there. The client's leaving
//             ends the session; in the next one, when the joiner's close comes back, it says DONE: that
//             session's lane sent nothing and applied the one close. Every end of its leg after the spawn
//             takes the PC out of the world again, a session that ends mid-leg too, but for the hold.
//   CLIENT -- from its own world-ready: waits for a portable PC of its own to open -- the host's, since
//             every other copy loads closed -- then closes the lid with the PC's action 11. Its DONE
//             line says it applied the one open and sent the one close, so nothing it applied went back
//             out. A joiner runs the same leg on the open lid its row carried.
// "[lid_drill] FAIL" is the lane failing a step (a run's --fail-marker), "[lid_drill] ABANDONED" the
// drill unable to do its part (--dead-marker). Run with lid_drill=1 and --rejoin: the first client's DONE
// line is the rejoin marker, the host's DONE line the done marker. Under --rehost the host's "held the
// lid open" line ends the first world, and the client's DONE line in the second ends the run.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::lid_drill {

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// The per-session half: a rejoin's peers run their legs again.
void OnDisconnect();

}  // namespace coop::dev::lid_drill
