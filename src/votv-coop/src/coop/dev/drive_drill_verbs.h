// coop/dev/drive_drill_verbs.h -- the player's own verbs the drive drill drives, each the game's own, as a player's
// input reaches it: the pocket, the take-out from the inventory, the hand's pickup and throw. A private header of
// coop/dev/drive_drill.cpp, coop/dev/throw_drill.cpp, coop/dev/prop_act_drill_client.cpp and
// coop/dev/explosion_drill_source.cpp (src tree, not include/).

#pragma once

#include <string>

namespace coop::dev::drive_drill_verbs {

// mainPlayer.addObjectToInventory: `prop` into the player's inventory, the world actor gone. True when it was taken.
// A client's call on a mirror of a shared prop is cancelled and asked of the host: false at once, with
// prop_act_request::dev::PendingId() != 0, and the act ends a round trip later (the caller waits on that, then reads
// the inventory).
bool Pocket(void* player, void* prop);

// The player container's getObject for the carried record of class `cls` and key `key`. The game's getObject has an
// empty body, so this answers null whenever it runs: the drive drill's take-out step has no game route to drive.
void* TakeOut(const std::wstring& cls, const std::wstring& key);

// mainPlayer's Add Object to Free Hotbar Slot with `prop` as its target: the hand's pickup, the world actor gone and
// the item in hand.
// True when it was collected. A client's call on a mirror of a shared prop is asked of the host as Pocket's is; the
// caller then reads the hand once PendingId() is 0.
bool Hold(void* player, void* prop);

// mainPlayer.throwHoldingProp: its place-drop of the selected hotbar slot destroys the hand's item and spawns a new
// world actor from the slot's data, which the throw then launches. False when unresolved.
bool Throw(void* player);

}  // namespace coop::dev::drive_drill_verbs
