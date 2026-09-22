// coop/dev/food_clock_probe.h -- what a food's save record does to its own state on arrival.
//
// Aprop_food_C::loadData does not simply restore temperature and ripeness: it runs a CATCH-UP
// against a timestamp the sender wrote with its own UGameplayStatics::GetTimeSeconds, and both
// branches compute `my GetTimeSeconds() - the sender's`. Two peers' worlds start at different
// moments, so that difference is not the elapsed time it is read as. This prints what the sender
// sent, what the receiver ended up with, and the difference of the two clocks that produced it.
//
// The post-apply read is the actor's own getData called again, so the numbers come from the game's
// codec rather than from a field offset kept here, and the receiver's clock arrives with them in
// the record's own stamp.
//
// Read-only. Nothing about what crosses changes with it on. Ini-gated [dev] food_clock_probe=1;
// set it on BOTH peers -- the host prints the publish half and the joiner the apply half.

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

// The tally and the reading. Printed on a period once anything is recorded, because the rig kills
// its peers rather than disconnecting them.
void Tick();

}  // namespace coop::dev::food_clock_probe
