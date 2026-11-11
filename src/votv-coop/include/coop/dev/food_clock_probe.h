// coop/dev/food_clock_probe.h -- whether the food family stays off the prop-record lane.
//
// A food keeps its temperature and ripeness in state that ages with the world's clock, and the food lane
// (coop/props/prop_food_state) owns it: the prop-record lane declares the family claimed and turns every
// food away at its class gate. The record seams sit BEHIND that gate, so on a shipping build the publish and
// apply counts stay zero and what this reads there is the gate's REFUSALS, which is the claim measured
// rather than inferred from silence.

#pragma once

namespace coop::dev::food_clock_probe {

bool IsEnabled();

// A record about to leave this peer, at a send seam. Counts only a food.
void NotePublish(void* actor);

// A record that has just been applied to `actor`, at the one apply seam. Counts only a food.
void NoteApply(void* actor);

// The record lane's class gate turned `actor` away, at any of its refusal points. Counted
// only for a food, so the tally is the ownership claim doing its job.
void NoteRefused(void* actor);

// The tally and the reading. Printed on a period once anything is recorded, because the rig kills
// its peers rather than disconnecting them.
void Tick();

// Drop the cached class and the tallies. A Blueprint class can be unloaded across a level change,
// and a verdict that spans two sessions is the sum of two runs.
void OnDisconnect();

}  // namespace coop::dev::food_clock_probe
