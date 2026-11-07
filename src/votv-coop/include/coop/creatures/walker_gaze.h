// coop/creatures/walker_gaze.h -- a client's gaze on the walking mannequin that hunts it.
//
// A walker freezes on its TARGET's gaze only (coop/creatures/walker_gaze_host answers the host's own gaze reads with the level this
// module sends). The host names the target in WalkerState (coop/creatures/walker_state_client keeps it per copy), and when a copy
// hunts THIS client, this module judges the game's own test on that copy, `isBoxInFrustum(player controller 0, the mesh's
// bounds) && mesh.WasRecentlyRendered(3)`, the same two natives the walker's tick calls on the host and with the arguments its
// bytecode gives them, and sends the LEVEL (WalkerSeen {key, seen}): on each change of it and once when this client becomes the copy's
// target. A copy's end sends nothing: the copy ends because the host's walker ended, whose levels the host drops then.
//
// THE POLL. Rendering and the camera's frustum announce nothing: no engine signal says "this mesh is on screen now", so the
// judgment is read each frame, and only while a copy hunts this client. With none, the tick returns on one check. A client that
// stops being a copy's target sends nothing: the host cleared every level of that walker at the change of target, and a word from a
// client that is no longer the target would be refused. Coop layer; game thread.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::walker_gaze {

// Subscribe to the copies' hunt, once, from a client's session start. Game thread.
void Install(coop::net::Session& session);

// Per gameplay tick on a client: judge each copy that hunts this client, at any distance (the Blueprint's test has no range
// term), and send a level that changed. Returns on one comparison while no copy does. Game thread.
void Tick();

// The session ended: nothing is held and nothing is sent. Game thread.
void OnDisconnect();

}  // namespace coop::walker_gaze
