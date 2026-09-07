// ue_wrap/devices/garage.h -- engine access for the base GARAGE door (Agarage_C), the principle-7
// wrapper layer: no network or coop state. coop::interactable_sync drives the sync through here.
//
// Agarage_C is a level-placed AtriggerBase_C descendant. Unlike Adoor_C it has no sensor, no
// autoclose and no tick, so it never reverts on its own and syncs SYMMETRICALLY. The wall button
// only fires runTrigger and the garage toggles itself, so we read and write the garage's own open
// flag, which catches every activation source, and never the button.
//
// Identity is the garage's LEVEL-EXPORT FName, its placed-actor name, and not the inherited
// AtriggerBase_C key. That key comes from the gamemode's one-shot, sublevel-gated keying pass
// (loadObjects -> loadTriggers, gated on isSublevelAllowed), and a garage that misses the pass
// keeps the class default -- "garageDoor", which every garage instance shares and which therefore
// cannot tell two of them apart. The host was seen losing its garage identity across a reload while
// every door kept its key. The level-export FName is baked into the cooked map package, so both
// peers read the identical name -- the same reason door_box keys the lockers by name.

#pragma once

#include <string>

namespace ue_wrap::garage {

// Resolve Agarage_C + the Open offset + the acivae UFunction. Idempotent; true
// once resolved. Game thread.
bool EnsureResolved();

// True iff `obj`'s class is garage_C or a subclass. False if not yet resolved.
bool IsGarage(void* obj);

// The garage's LEVEL-EXPORT FName (its placed-actor name) as a wide string ("" on failure).
// This is the cross-peer identity -- see the header note on why NOT the save Key.
std::wstring GetNameKey(void* g);

// Read the garage's `Open` bool into `open`. False if the read could not be made (null /
// not resolved); leaves `open` untouched on failure.
bool TryReadOpen(void* g, bool& open);

// Drive the garage to `open` -- idempotent (a no-op if it is already there). Writes the `Open` flag
// directly, because neither settime nor acivae writes it -- only runTrigger and loadTriggerData do
// -- and the mirror's poll baseline would otherwise go stale and oscillate. Then plays the native
// animated swing through acivae(): the montage from position 0 at half rate, and the move timeline
// over its full length, both taking their direction from the flag just written. NOT settime, which
// runs that same timeline and then snaps it -- the montage at full rate from position 100, then
// SetNewTime straight to the endpoint. Must run on the game thread. False on null or an unresolved
// UFunction.
bool ApplyOpen(void* g, bool open);

}  // namespace ue_wrap::garage
