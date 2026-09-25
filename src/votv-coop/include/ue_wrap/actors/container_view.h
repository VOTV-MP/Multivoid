// ue_wrap/actors/container_view.h -- the local player's view into a container: the inventory screen
// mainGamemode_C::openPropInv opens (the gamemode's propInventory, a ui_playerInventory_C), which container it
// shows (its `entered`, which gen_player sets as openPropInv opens the screen) while it is the player's
// active interface, and the screen's own close: exit() (the interface cleared, the screen hidden, the close
// sound), then the first tab again and the selection redrawn, as the screen's close key runs it. Game thread.
// Engine-wrapper layer (principle 7): no gameplay logic.
#pragma once

namespace ue_wrap::container_view {

// The container the local player's inventory screen shows, or null when that screen is not the
// player's active interface (closed, or another interface is up).
void* Viewed();

// Whether `container` is the local player's own inventory (the gamemode's playerContainer, which the
// inventory key opens, or any container of the player-inventory class, as the screen itself tells them
// apart). A view onto it is opened through no actor.
bool IsOwnInventory(void* container);

// Whether the open screen's close resolves; false when no screen is open.
bool CanClose();

// Open the screen onto `container` (an Aprop_container_C) through the gamemode's own openPropInv, as
// the actor that opens it does. False when the gamemode or the verb cannot be reached.
bool Open(void* container);

// Close the screen as its close key does. False, and nothing done, when it is not open or its exit()
// does not resolve.
bool Close();

}  // namespace ue_wrap::container_view
