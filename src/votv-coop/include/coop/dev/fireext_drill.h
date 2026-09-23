// coop/dev/fireext_drill.h -- drill: a fire extinguisher taken off its wall mount (ini
// fireext_drill=off|carry|short|client|join / env VOTVCOOP_FIREEXT_DRILL; BOTH peers).
//
// One peer acts, both watch. The actor walks to the nearest mounted extinguisher by the bot
// director, turns the camera until the game's own trace takes it, and grabs it through the chain
// the use key's release runs (playerGrabbed_pre, useAction, playerGrabbed). carry: the host, after
// a client's join and join window, carries it some metres and lets go; the client runs the laptop
// exit's setPropProps once on its carried copy, which the drive must re-latch. short: the host
// lets go the moment it holds it. client: the client carries, the host watches. join: the host
// takes it off while a joiner's captured world still loads. Each step waits on a state the actor
// reads; the last prints `ACTOR DONE`, a refused one `INVALID` with its reason. The evidence is the
// watch: each peer prints every extinguisher it has, then each move or change of its frozen,
// mounted, thrusting or spraying bit, so the two logs name one key.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::fireext_drill {

// True unless the row is `off`.
bool IsEnabled();

// Advance this peer's steps and its watch. Every pump tick in a world. Game thread.
void Tick(coop::net::Session* session);

// The session ended: back to the first step, the watch emptied.
void OnDisconnect();

}  // namespace coop::dev::fireext_drill
