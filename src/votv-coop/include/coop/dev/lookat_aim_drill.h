// coop/dev/lookat_aim_drill.h -- put a resting prop under a peer's crosshair and leave it there, so
// the look-at churn probe beside it has something to read.
//
// The flicker the probe measures only shows while an interactable is under the crosshair, and a peer
// in a rig run aims at nothing. This drill supplies that one precondition and no more: it turns the
// camera through a fan of headings until the game's OWN trace resolves a prop, says so, and then
// stops. Nothing is pressed, nothing is written to the prop, and the camera is not touched again
// once the trace has taken something, so every change the probe counts afterwards belongs to the
// peer rather than to the drill. A peer standing where no prop is visible gets no reading and the
// log says which it was; the drill does not walk somewhere better, because a chosen actor is not a
// reachable one and the trace, not the drill, decides what an aim can land on.
//
// Run it on BOTH peers of a pair: the defect is reported on clients and not on the host, so the
// host's reading is the control arm, and only a run where both are aimed produces the comparison.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::lookat_aim_drill {

bool IsEnabled();

// Advance the drill one step. Game thread, once per pump tick; a single bool read when off, and one
// reflected getter per tick only while it is still sweeping.
void Tick(coop::net::Session* session);

// Drop the aim and the phase, so a rejoin sweeps the new world instead of holding a heading chosen
// for the old one.
void OnDisconnect();

}  // namespace coop::dev::lookat_aim_drill
