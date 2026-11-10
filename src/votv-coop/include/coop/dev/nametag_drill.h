// coop/dev/nametag_drill.h -- [dev] a prop's nametag is the host's, on every peer.
// nametag_drill=run: once slot 1's world is ready the host spawns a cup before the client's puppet; the client takes a
// nametag into its hand (the hold slot and the game's updateHold), writes the drill's tag on it, walks to the cup, aims
// until its trace takes it and runs the item's own left click. Host DONE: its cup carries the tag, applied for a
// client once. Client DONE: its copy carries the tag and the item in its hand is spent. nametag_drill=host: the host
// player tags the cup the same way; client DONE when its copy carries the host's tag.
// The red: nametag_client_runs=1 on the client runs the game's own use there, so the tag stays on its copy and the
// host FAILs. "[NAMETAG-DRILL] FAIL" is the lane failing, "ABANDONED" the drill unable to do its part; the host's DONE
// (run) or the client's (host) ends a run.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::nametag_drill {

// Advances this peer's steps; a latched read when off. Game thread.
void Tick(coop::net::Session* s);

// The session ended: the drill starts over.
void OnDisconnect();

}  // namespace coop::dev::nametag_drill
