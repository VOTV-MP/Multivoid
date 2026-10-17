// coop/dev/hook_plant.h -- a player's hook planted on a prop by hand, as the item does it: a mirror hook spawned at the
// prop, attach_a with the head on the prop's mesh and the tail on the player, the field the item writes. Dev only.

#pragma once

namespace coop::dev::hook_plant {

// The planted hook, or null with `*note` set ("no-static-mesh", "target-location-unread", "hook-did-not-spawn",
// "attach_a-did-not-dispatch"); "planted" on success. Game thread.
void* Plant(void* player, void* target, const char** note);

}  // namespace coop::dev::hook_plant
