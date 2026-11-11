// ue_wrap/actors/container_view.h -- the local player's view into a container: the inventory screen
// (ui_FreeLayoutInventory_C, mainGamemode_C::ui_FLI_inventory) that a container's opencontainer, an ATV's and a
// drone's open onto their inventory component with begin(component), and the screen's own close, end(). The screen
// shows two canvases, the player's bag and the open component's; dragging an item between them is the only way a
// player takes from or puts into a container, and the screen moves it with the component's removeInventoryItem and
// addItemAdvanced. This module reads which container the screen shows and finds and discards the copy a drag has just
// filed in the player's bag. Game thread.
// Engine-wrapper layer (principle 7): no gameplay logic.
#pragma once

#include "ue_wrap/actors/inventory_item.h"

#include <cstdint>

namespace ue_wrap::container_view {

// The screen, live, or null when the gamemode or the field does not resolve.
void* Screen();

// The container the screen shows: the owner of the screen's open component while the screen is active, when that
// owner is a prop_container_C. Null when the screen is closed or shows an ATV's or a drone's inventory or the
// player's own bag alone.
void* Viewed();

// The inventory component the active screen shows beside the player's bag, whoever owns it (a container, a drone, an ATV);
// null when the screen is closed.
void* ViewedInventory();

// Open the screen onto an inventory component through the screen's own begin(component), as an ATV's open option does.
// False when the screen or the verb does not resolve or the call fails.
bool OpenInventory(void* component);

// Drop the bag's item widget `bagItem` on the canvas of the component the screen shows, as a mouse release over that
// canvas does: the screen's currentlyDragging is set to the widget, hoverContainer is raised and enddrag runs (which
// files the entry with the component's addItemAdvanced from the screen's own script). False when a field or the verb
// does not resolve, or the call fails; the drop's outcome is the caller's to read.
bool DropOnViewed(void* bagItem);

// Whether the active screen's close resolves.
bool CanClose();

// Close the screen as its close key does (end). False, and nothing done, when it is not active or end() does not resolve.
bool Close();

// Open the screen onto `container` through its own opencontainer, as the player's use of it does. False when the
// verb does not resolve or the call fails.
bool Open(void* container);

// The item widget a drag has just filed in the player's bag as the copy of `taken`: the one the screen holds as its
// currentlyDragging or in its mass selection whose entry is `taken`'s item (class, name, tag, keys, state and amount)
// and whose owner is not `from`, the component `taken` is leaving. Null when there is none.
void* CopyOf(const ue_wrap::inventory_item::Item& taken, void* from);

// The player's own bag component the screen shows (playerInventoryComponent), or null.
void* PlayerComponent();

// One item widget as the screen holds it: the entry it shows (its dataReference), the inventory component that owns it
// and the entry's index in that component's array.
struct WidgetEntry {
    ue_wrap::inventory_item::Item item;
    void* owner = nullptr;
    int32_t index = -1;
};
// False when the widget is not live or its layout does not resolve.
bool ReadWidget(void* widget, WidgetEntry& out);

// The widget the screen holds as currentlyDragging whose entry is `item` (the same item: class, name, tag, keys, state and
// amount, wherever it lies), or one of the mass selection's; null when there is none. `CopyOf` with no component to exclude.
void* SourceOf(const ue_wrap::inventory_item::Item& item);

// Take the copy back out of the bag: the widget's own removeItem (which blanks its entry in its owner and removes
// the widget) and, when the screen is dragging it, the screen's dropDragged. False when removeItem does not resolve.
bool Discard(void* copy);

}  // namespace ue_wrap::container_view
