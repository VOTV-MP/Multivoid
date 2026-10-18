// coop/dev/hook_plant.h -- a player's hook planted on a prop by hand, as the item does it: a mirror hook spawned at the
// prop, attach_a with the head on the prop's mesh and the tail on the player, the field the item writes. Dev only.

#pragma once

namespace coop::dev::hook_plant {

// The planted hook, or null with `*note` set ("no-static-mesh", "target-location-unread", "hook-did-not-spawn",
// "attach_a-did-not-dispatch"); "planted" on success. Game thread.
void* Plant(void* player, void* target, const char** note);

// The item's cancel for a hook planted by hand: the field Plant wrote on `player` is cleared whether or not the hook
// still stands, then the hook goes. Either argument may be null. Game thread.
void Release(void* player, void* hook);

}  // namespace coop::dev::hook_plant
