// ue_wrap/hotbar/icons.h -- the hotbar and the two tables its icons are drawn from.
//
// The hotbar is the inventory screen's (`mainGamemode.ui_FLI_inventory`, a ui_FreeLayoutInventory_C) row of
// `hotbarSlots`, each a uicomp_FLI_itemSlot_C. A slot's own `upd` draws its icon: it writes the `tex` parameter of
// its `image_item` material from `lib_C::propToIcon`, which indexes a texture array on the game instance by a name
// table on `ApropProcessor_C`. The renderer publishes that pair seconds after the world is up, and a slot drawn
// before then gets a null texture and keeps it until something redraws it. docs/ui.md has the mechanism and what the
// mod adds.
//
// This file is the wrapper half: read the bar's inputs, read what a slot is showing, and re-issue the game's verb.
// Which world is owed a redraw, and when, is a policy and lives in coop/items/hotbar_icon_edge.h.
//
// Engine-wrapper layer (principle 7): reflection and thunks only, no gameplay policy, no
// network and no coop state. Game thread.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::hotbar {

// The bar and its icon source as they stand right now. Every field is a count read straight off
// the live objects, so a caller can say WHICH input was missing rather than that one was.
struct State {
    void*   ui             = nullptr;  // ui_FreeLayoutInventory_C (mainGamemode.ui_FLI_inventory)
    void*   renderer       = nullptr;  // ApropProcessor_C (mainGamemode.propRenderer)
    int32_t slots          = 0;        // ui_FLI_inventory.hotbarSlots
    int32_t filled         = 0;        // of those, the slots whose slotItemName names an item
    int32_t firstFilled    = -1;       // the first such slot, -1 when none
    int32_t iconNames      = 0;        // propProcessor.names          (propToIcon's index side)
    int32_t iconTextures   = 0;        // mainGameInstance.texs_GAMEINST (propToIcon's value side)
    int32_t rendererPhases = -1;       // propProcessor.fins -- how far the render got

    // Both halves of the icon lookup are published. Either one empty makes every icon null.
    bool IconsReady() const { return iconNames > 0 && iconTextures > 0; }
};

// Read it. False (and `out` left alone) before the world is up or while the reflection has not
// resolved. Pure field reads, no UFunction dispatch -- safe from inside a Blueprint body.
bool Read(State& out);

// The texture bound to slot `i`'s `tex` material parameter, by name. Empty when the slot index is
// out of range or the widget is gone; `no-material` when the image was never touched by a
// draw; `no-parameter` when a material is bound but carries no `tex` override. A draw that
// found an icon leaves a texture name here; one that did not leaves `null`.
std::wstring SlotTexture(const State& s, int i);

// Redraw every hotbar slot through its own `upd`. Idempotent by construction (the verb draws from the slot's item
// every time). False if the verb or the widget is unavailable, or a slot's call fails.
bool Rebuild(const State& s);

}  // namespace ue_wrap::hotbar
