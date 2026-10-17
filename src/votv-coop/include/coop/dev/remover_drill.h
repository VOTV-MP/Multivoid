// coop/dev/remover_drill.h -- [dev] the tool gun's remover pressed by a client, each peer judging its own copies
// (ini remover_drill=granted|withheld, env VOTVCOOP_REMOVER_DRILL; BOTH peers, the same value).
//
//   HOST   -- once slot 1's world is ready, spawns sacks about the puppet (props the remover may remove) and, in
//             `granted`, a kerfurOmega (a creature it may not) and two crowbars, plants its own hook on the first
//             and says when the claim pass reads it. It judges its copies and its own count of answers when the
//             client's closing chat line reaches it.
//   CLIENT -- finds the host's props, spawns a toolgun and, per shot, a tool_remover_C; writes the toolgun's outHit
//             at the target and runs init(toolgun), the body a press runs. It waits on what its copies and screen show.
//             granted:  one prop; six presses at once on another; every ring prop in its sight (ten at least), one
//                       after another, none refused for sight; the creature, which stays and is answered; the crowbar
//                       the host's hook pulls (refused as held); the one its own hook pulls (removed).
//             withheld: one prop the host's permissions refuse: it stays and the refusal shows.
// A FAIL is the lane failing, an ABANDONED the drill unable to do its part; each leg prints "client leg". A removal
// the client did natively and the host merely applied cannot pass: the host's answered counts are part of its verdict.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::remover_drill {

// Advances this peer's steps; a latched read when off. Game thread.
void Tick(coop::net::Session* s);

// The session ended: the drill starts over.
void OnDisconnect();

}  // namespace coop::dev::remover_drill
