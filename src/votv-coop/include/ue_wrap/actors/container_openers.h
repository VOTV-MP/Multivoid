// ue_wrap/actors/container_openers.h -- the actors through which the game opens a container that
// stands somewhere else. A container is opened where it stands (prop_container_C opens itself), but
// three kinds of actor open one that does not: the drone opens its inventory, a separate
// prop_inventoryContainer_drone_C kilometres off (drone_C.container); the sack it carries opens the same
// one (prop_dronesack_C.container, found by the key drone_InventoryContainer); the ATV opens its own,
// spawned under the world origin (ATV_C.spawnedContainer). Read from the bytecode: drone.cpp:1335,
// prop_dronesack.cpp:52 and :109, ATV.cpp:1399 and :6842-6856. The relation is the game's own field on
// the opener, never anyone's claim. Engine-wrapper layer (principle 7): no gameplay logic.
#pragma once

namespace ue_wrap::container_openers {

// Call `fn` for each live, readable actor whose opener field names `container`, until it answers
// false. The drones, sacks and ATVs come from the object index, a handful of each. Game thread.
using OpenerFn = bool (*)(void* ctx, void* opener);
void ForEach(void* container, OpenerFn fn, void* ctx);

// The container `opener` opens: its field's value when it is a drone, a sack or an ATV, else null.
// Game thread.
void* Opens(void* opener);

}  // namespace ue_wrap::container_openers
