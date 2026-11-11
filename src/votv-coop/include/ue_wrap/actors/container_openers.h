// ue_wrap/actors/container_openers.h -- the actors through which the game opens a container that
// stands somewhere else. A container is opened where it stands (prop_container_C opens itself), but
// the ATV opens one it spawns for itself (ATV_C.spawnedContainer), handing that field to
// mainGamemode_C::openPropInv from its own actionOptionIndex. The drone and its sack are not openers: the
// drone's cargo is its own inventory component and the sack is a container itself. The relation is the
// game's own field on the opener, never anyone's claim. Engine-wrapper layer (principle 7): no gameplay
// logic.
#pragma once

namespace ue_wrap::container_openers {

// Call `fn` for each live, readable actor whose opener field names `container`, until it answers
// false: an ATV, or an instance of a subclass of one, from the object index, a handful of them. Game thread.
using OpenerFn = bool (*)(void* ctx, void* opener);
void ForEach(void* container, OpenerFn fn, void* ctx);

// The container `opener` opens: its field's value when it is an ATV, else null.
// Game thread.
void* Opens(void* opener);

}  // namespace ue_wrap::container_openers
