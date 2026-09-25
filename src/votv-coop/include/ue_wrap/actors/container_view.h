// ue_wrap/actors/container_view.h -- the local player's view into a container: the inventory screen
// mainGamemode_C::openPropInv opens (the gamemode's propInventory, a ui_playerInventory_C), which container it
// shows (its `entered`, set by gen_player) while it is the player's active interface, and its own close,
// exit() (the interface cleared, the screen hidden, the close sound). Read from the bytecode:
// mainGamemode.cpp:10740-10757, ui_playerInventory.cpp:824-828 and :1502-1523. Game thread.
// Engine-wrapper layer (principle 7): no gameplay logic.
#pragma once

namespace ue_wrap::container_view {

// The container the local player's inventory screen shows, or null when that screen is not the
// player's active interface (closed, or another interface is up).
void* Viewed();

// The local player's own inventory container (the gamemode's playerContainer, which the inventory key
// opens: mainPlayer.cpp:6707), or null. A view onto it is opened through no actor.
void* OwnContainer();

// Open the screen onto `container` (an Aprop_container_C) through the gamemode's own openPropInv, as
// the actor that opens it does. False when the gamemode or the verb cannot be reached.
bool Open(void* container);

// Close the screen through the game's own exit(). False, and nothing done, when it is not open.
bool Close();

}  // namespace ue_wrap::container_view
