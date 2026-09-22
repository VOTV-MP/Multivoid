// coop/dev/lookat_aim_drill.h -- stand a peer in front of a resting prop and hold the aim on it,
// so the look-at churn probe beside it has something to read.
//
// The flicker the probe measures only shows while an interactable is under the crosshair, and a
// peer in a rig run aims at nothing. This drill supplies that one precondition and no more: it
// picks the nearest prop, walks to it with the bot director over the NavMesh -- never a teleport,
// which can land a player inside geometry and hand the run a broken session to measure -- turns
// the camera onto it, and then STOPS. Nothing is pressed, nothing is written to the prop, and the
// aim is not refreshed once the game's own trace has resolved it, so every change the probe counts
// afterwards belongs to the peer, not to the drill.
//
// Run it on BOTH peers of a pair: the defect is reported on clients and not on the host, so the
// host's reading is the control arm, and a run where both are aimed produces the two halves of
// that comparison in one pass.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::lookat_aim_drill {

bool IsEnabled();

// Advance the drill one step. Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// Drop the target and the phase, so a rejoin starts the drill over rather than holding an aim at
// an actor the new world does not have.
void OnDisconnect();

}  // namespace coop::dev::lookat_aim_drill
