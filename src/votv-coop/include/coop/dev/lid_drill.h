// coop/dev/lid_drill.h -- [dev] the portable PC's lid across both peers. The rig's save has no portable
// PC, so the host brings one in.
//   HOST   -- once a client's world is ready: spawns a PC in front of its player and, once the element
//             lane names it, opens the lid with the PC's own action 10, as a player's use does. When the
//             client's close reaches its copy (one line sent, one applied) it opens the lid again and
//             holds it for the next joiner, whose copy loads closed, so only the joiner's world-ready row
//             can open it. In the next session, when the joiner's close comes back, it says DONE: sent
//             nothing, applied the one close. Any end of its leg after the spawn but the hold removes the PC.
//   CLIENT -- from its own world-ready: waits for a portable PC of its own to open -- the host's, since
//             every other copy loads closed -- then closes the lid with action 11. Its DONE line says it
//             applied the one open and sent the one close. A joiner runs the same leg on the open lid.
// "[lid_drill] FAIL" is a lane step failing (--fail-marker), "[lid_drill] ABANDONED" the drill unable to
// do its part (--dead-marker). Run with lid_drill=1 and --rejoin (the first client's DONE rejoins, the
// host's DONE ends the run) or --rehost (the host's "held the lid open" line ends the first world, the
// client's DONE the run).

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::lid_drill {

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// The per-session half: a rejoin's peers run their legs again.
void OnDisconnect();

}  // namespace coop::dev::lid_drill
