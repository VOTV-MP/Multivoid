// coop/dev/fireext_drill.h -- drill: a player takes a fire extinguisher off its wall mount (ini
// fireext_drill=1 / env VOTVCOOP_FIREEXT_DRILL; BOTH peers).
//
// The HOST acts and both peers watch. Once a client's join and its join window are over, the host
// picks the mounted extinguisher nearest to it, walks there by the bot director, turns the camera
// until the game's own trace takes it, and grabs it through the chain the use key's release runs:
// the prop's playerGrabbed_pre, which unfreezes a frozen prop, the player's useAction, the prop's
// playerGrabbed. It carries it to a NavMesh-reachable point some metres off, lets go, and waits for
// it to rest. Every step waits on a state the host reads, and the last one prints `HOST DONE`; a
// refused step prints `INVALID` with its reason.
//
// The watch is the evidence: at its join's end each peer prints every extinguisher it has, and
// again whenever one moves or its frozen or mounted bit changes, so the two logs name one key.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::fireext_drill {

// True when the row is set.
bool IsEnabled();

// Advance this peer's steps and its watch. Every pump tick in a world. Game thread.
void Tick(coop::net::Session* session);

// The session ended: back to the first step, the watch emptied.
void OnDisconnect();

}  // namespace coop::dev::fireext_drill
