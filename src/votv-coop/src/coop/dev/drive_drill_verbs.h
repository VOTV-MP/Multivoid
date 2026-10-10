// coop/dev/drive_drill_verbs.h -- the player's own verbs the drive drill drives, each the game's own, as a player's
// input reaches it: the pocket, the take-out from the inventory, the hand's pickup and throw. A private header of
// coop/dev/drive_drill.cpp (src tree, not include/).

#pragma once

#include "ue_wrap/core/types.h"

#include <string>

namespace coop::dev::drive_drill_verbs {

// mainPlayer.putObjectInventory2: `prop` into the player's inventory, the world actor gone. True when it was taken.
bool Pocket(void* player, void* prop);

// The player container's getObject for the carried record of class `cls` and key `key`: the inventory screen's take-out,
// which spawns the item at the player and loads its record. The spawned actor, or null.
void* TakeOut(const std::wstring& cls, const std::wstring& key);

// mainPlayer's "Hold Object" with `prop` as its target: the hand's pickup, the world actor gone and the item in hand.
// True when it was collected.
bool Hold(void* player, void* prop);

// mainPlayer.throwHoldingProp: its simulateDrop destroys the hand's item and spawns a new world actor of the hold
// slot's class, which the throw then launches. False when unresolved.
bool Throw(void* player);

}  // namespace coop::dev::drive_drill_verbs
