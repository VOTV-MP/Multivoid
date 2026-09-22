// coop/dev/food_clock_probe.h -- what a food's save record does to its own state on arrival.
//
// Aprop_food_C::loadData does not simply restore temperature and ripeness: it runs a CATCH-UP
// against a timestamp the sender wrote with its own UGameplayStatics::GetTimeSeconds, and both
// branches compute `my GetTimeSeconds() - the sender's`. Two peers' worlds start at different
// moments, so that difference is not the elapsed time it is read as.
//
// The post-apply read is the actor's own getData called again, so the numbers and the receiver's
// clock both come from the game's codec rather than a field offset kept here. It costs one extra
// dispatch per applied record, over the two the lane's apply budget is sized for.
//
// The publish and apply seams sit BEHIND the lane's class gate, and the food family is claimed off
// that lane, so on a shipping build they never fire: what this reads there is the gate's REFUSALS,
// which is the claim measured rather than inferred from silence.

#pragma once

#include <string>

namespace ue_wrap::save_record { struct SaveRecord; }

namespace coop::dev::food_clock_probe {

bool IsEnabled();

// A record about to leave this peer, at the one send seam. Prints only for a food.
void NotePublish(void* actor, const std::wstring& key, const ue_wrap::save_record::SaveRecord& rec);

// A record that has just been applied to `actor`, at the one apply seam. Re-captures the actor to
// read what the catch-up left behind. Prints only for a food.
void NoteApply(void* actor, const std::wstring& key, const ue_wrap::save_record::SaveRecord& sent);

// The record lane's class gate turned `actor` away, at any of its three refusal points. Counted
// only for a food, so the tally is the ownership claim doing its job.
void NoteRefused(void* actor);

// The tally and the reading. Printed on a period once anything is recorded, because the rig kills
// its peers rather than disconnecting them.
void Tick();

// Drop the cached class and the tallies. A Blueprint class can be unloaded across a level change,
// and a verdict that spans two sessions is the sum of two runs.
void OnDisconnect();

}  // namespace coop::dev::food_clock_probe
