// ue_wrap/actors/container_view.h -- the local player's view into a container: the inventory screen
// mainGamemode_C::openPropInv opens (the gamemode's propInventory, a ui_playerInventory_C), which container it
// shows (its `entered`, which gen_player sets as openPropInv opens the screen) while it is the player's
// active interface, and the screen's own close: exit() (the interface cleared, the screen hidden, the close
// sound), then the first tab again and the selection redrawn, as the screen's close key runs it. Game thread.
// Engine-wrapper layer (principle 7): no gameplay logic.
#pragma once

#include <cstdint>

namespace ue_wrap::container_view {

// The container the local player's inventory screen shows, or null when that screen is not the
// player's active interface (closed, or another interface is up).
void* Viewed();

// Whether `container` is the local player's own inventory (the gamemode's playerContainer, which the
// inventory key opens, or any container of the player-inventory class, as the screen itself tells them
// apart). A view onto it is opened through no actor.
bool IsOwnInventory(void* container);

// The gamemode's playerContainer, the actor of the local player's own inventory; null when it does not resolve or is
// not live. Its records read through container_inventory::InventoryOf like any container's.
void* OwnInventory();

// Whether the open screen's close resolves; false when no screen is open.
bool CanClose();

// Open the screen onto `container` (an Aprop_container_C) through the gamemode's own openPropInv, as
// the actor that opens it does. False when the gamemode or the verb cannot be reached.
bool Open(void* container);

// Close the screen as its close key does. False, and nothing done, when it is not open or its exit()
// does not resolve.
bool Close();

// One container slot of the inventory screen: its screen (owner), the container that screen takes from (the
// screen's as_prop; null on the player's own pane, which is still a slot) and the record index it names (id).
struct Slot {
    void* screen;
    void* container;
    int32_t id;
};

// Read `obj` as a slot. False when it is not a uicomp_playerInvContainerSlot_C or a field does not resolve.
bool ReadSlot(void* obj, Slot& out);

// The slot's own pressButton. False when it does not resolve or the call fails.
bool PressSlot(void* slot);

// When the open screen shows `container`: gen_player(enter = container, double = true), as pressButton's own
// redraw does. False, and nothing done, when the screen is closed, shows another container, or the call fails.
bool Redraw(void* container);

// What the `hotbar_1` key's em_take reads before it puts the selected record of the player's own pane into the
// container the screen shows: that container (as_prop; null on the player's own pane), whether the screen's take
// button is enabled, whether a container slot is hovered (a live hoverContainerSlot) and visible, and the selected
// record's index (the screen's own getInd).
struct PutState {
    void* container;
    bool takeEnabled;
    bool hovered;
    bool hoverVisible;
    int32_t index;
};

// Read `screen` (a ui_playerInventory_C) as the put reads it. False, `out` zeroed, when it is not live or a field or
// a call does not resolve.
bool ReadPutState(void* screen, PutState& out);

// The screen's em_take, the hotbar_1 key's verb. False when it does not resolve or the call fails.
bool RunPut(void* screen);

// The gamemode's playerContainer, the player's own inventory actor; null when the gamemode or the field does not resolve.
void* PlayerContainer();

}  // namespace ue_wrap::container_view
