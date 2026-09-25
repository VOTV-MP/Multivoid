// ue_wrap/actors/container_openers.h -- the actors through which the game opens a container that
// stands somewhere else. A container is opened where it stands (prop_container_C opens itself), but
// three kinds of actor open one that does not: the drone opens its inventory, a separate
// prop_inventoryContainer_drone_C standing far from it (drone_C.container); the sack it carries opens the
// same one (prop_dronesack_C.container, found at its begin-play by the key drone_InventoryContainer); the
// ATV opens one it spawns for itself (ATV_C.spawnedContainer). Each hands that field to
// mainGamemode_C::openPropInv from its own actionOptionIndex. The relation is the game's own field on the
// opener, never anyone's claim. Engine-wrapper layer (principle 7): no gameplay logic.
#pragma once

namespace ue_wrap::container_openers {

// Call `fn` for each live, readable actor whose opener field names `container`, until it answers
// false: a drone, a sack or an ATV, or an instance of a subclass of one, from the object index, a
// handful of each. Game thread.
using OpenerFn = bool (*)(void* ctx, void* opener);
void ForEach(void* container, OpenerFn fn, void* ctx);

// The container `opener` opens: its field's value when it is a drone, a sack or an ATV, else null.
// Game thread.
void* Opens(void* opener);

}  // namespace ue_wrap::container_openers
