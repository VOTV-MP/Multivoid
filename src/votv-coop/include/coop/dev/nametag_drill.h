// coop/dev/nametag_drill.h -- [dev] a prop's nametag is the host's, on every peer.
// nametag_drill=run: the host spawns a cup before the client's puppet once the client has taken its snapshot of the cups
// that stand; the client walks to its copy with the director, takes a nametag into its hand, writes the drill's tag on
// it, waits for the hand lane to announce it and runs the item's own left click. nametag_drill=host: the host player
// tags the cup the same way once the client holds its copy. nametag_drill=window: the host tags a cup before the client
// joins, between the client's save being cut and its world being ready. The client's half holds when its copy carries
// the tag (run: the host's answer to its click came and the nametag is spent; window: at the join's end).
// The host alone says the verdict, `rig: VERDICT nametag PASS|FAIL|INVALID`; the client tells it `done ok|fail|invalid`
// in the lobby chat. Every verdict is an event; a bound only says INVALID.
// The reds: nametag_client_runs=1 on the client runs the game's own use there, so its click asks the host nothing and
// it says fail at once; nametag_no_join_replay=1 on the host (window mode) leaves the joiner's copy untagged.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::nametag_drill {

// Advances this peer's steps; a latched read when off. Game thread.
void Tick(coop::net::Session* s);

// The session ended: the drill starts over.
void OnDisconnect();

}  // namespace coop::dev::nametag_drill
