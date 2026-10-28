// coop/dev/cord_drill.h -- [dev] a cord plugged on either peer is plugged the same on both, by the host's word. Set
// cord_drill=1 on BOTH peers; run it on a fresh host world. The HOST spawns two coffee makers (A, B) by three free
// wall sockets (1, 2, 3) and says their keys in the lobby chat; each peer's player walks to a socket by the director
// and plugs, drags and holds a cord through the game's own verbs. Seven arms, each opening on a chat edge and closing
// when both peers print what they see:
//   1 host plugs A into 1   2 client plugs B into 2   3 client drags B into A's socket (B stays unplugged)
//   4 host holds A, client drags its copy into 3 (the host answers with its own link), host releases A into 1
//   5 client holds B, host plugs its B into 2 by a scripted Connect (held behind the drag), client releases in the air
//   6 client moves its own copy of A by a scripted actor move: the copy's break is refused (the stretch counter)
//   7 host moves its copy of A by a scripted actor move: its copy breaks and the client's copy is unplugged
// The client then prints `[CORD] client arms 1-7 PASS` (the rig's rejoin marker); while it is gone the host plugs A and
// unplugs B, and the rejoined client takes the host's expectation line and prints `[CORD] client DONE -- PASS`.
// The peers stand on either side of a socket's stand point, never on one spot. An arm's bound starts when its walking
// peer has arrived. A line starting `[CORD] FAIL:` is an arm not closed within its bound, a walk that did not arrive
// (`FAIL: <arm>: walk: ...`), or a step that cannot be driven (--fail-marker).

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::cord_drill {

// Store the session; idempotent, called at install. A latched read when off.
void Install(coop::net::Session* s);

// Advances this peer's steps; a latched read when off. Game thread.
void Tick();

// The session ended: the client starts over; the host starts over too, unless its seven arms are done and the client
// left, when it keeps its cast for the rejoin.
void OnDisconnect();

}  // namespace coop::dev::cord_drill
