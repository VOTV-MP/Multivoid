// coop/dev/swinger_drill.h -- [dev] what a dragged hinge door (prop_swinger_C) shows on the other peers: the game
// leaves a dragged swinger in the player's grabbing_actor, so the held-item stream carries its pose.
//   ini swinger_drill=off|measure|observe|rejoin / env VOTVCOOP_SWINGER_DRILL.
//   measure -- the host and the acting client (the first client). The client walks (bot director) to a non-lockable
//              lid its NavMesh reaches, one whose game Key differs between processes first, and grabs it through the
//              player's own grab chain. It feeds the player's input_mouseXY dispatcher, the delta doubling to a cap
//              on each sign in turn ((-,-), (+,-), (-,+), (+,+): a lid's spring holds it at its open limit) until the
//              door has turned. It lets go and, once its door has come to rest (the stream releases it only then),
//              says so in the lobby chat with the rotation and place it rests at; its DONE follows the host's verdict.
//   observe -- a second client (and any later one), which only watches. rejoin -- measure, then swinger_drill_join.h.
// The client's chat lines (grab with the lid's two keys, settled, each on a state edge) and the poses a watching
// peer takes are what it judges on: PASS when its copy of the door turned 10 degrees during the drag and, at rest,
// lies within 3 degrees (the angle between the quaternions) and 5 cm of the client's, as do the two doors' parent
// props when both are read, and the host's parent that simulates has 3 rows admitted from the client; a client door
// that did not turn 15 degrees is INVALID. Lines are tagged [SWING-DRILL]: a baseline per lid, a sample per move.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; struct PropPoseSnapshot; }

namespace coop::dev::swinger_drill {

// True unless the row is `off`.
bool IsEnabled();

// Game thread, once per pump tick; a single check when off.
void Tick(coop::net::Session* session);

// The session ended: the lids, the walk and the counters belong to one world.
void OnDisconnect();

// Seams of the held-prop stream, called by local_streams (the sender) and remote_prop (the receivers). Each is
// a predicate test and a return when the drill is off. Game thread.
//   NoteReleased       -- the release of `heldActor` was sent: the stream has let go of it.
//   NotePropPose       -- a PropPose from `slot` drove `actor` (its mesh `mesh`) toward the target pose.
//   NotePropUnresolved -- a PropPose from `slot` named a key and eid this peer resolved to nothing.
void NoteReleased(void* heldActor);
void NotePropPose(int slot, const coop::net::PropPoseSnapshot& pose, void* actor, void* mesh);
void NotePropUnresolved(int slot, const coop::net::PropPoseSnapshot& pose);

}  // namespace coop::dev::swinger_drill
