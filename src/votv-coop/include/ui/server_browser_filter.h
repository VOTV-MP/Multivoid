// ui/server_browser_filter.h -- the server browser's "Hide incompatible servers" checkbox.
//
// Checked, the list leaves out every lobby the join gate would refuse (ui/server_browser_rows owns that test); unchecked, they
// show with their red mark. The choice is remembered in browser.hide_incompatible, and the status pane says how many are hidden,
// so a filtered list never reads as an empty master. The state is here and any surface reads it; the widgets are the native
// browser's, spawned from the kit, as the game's font has no check glyph.

#pragma once

namespace ui::server_browser_filter {

// Whether the list leaves incompatible lobbies out. Any thread.
bool HideIncompatible();

// Sets it and remembers it in the ini. Repaints the box if it is built, so a game-thread call.
void SetHideIncompatible(bool hide);

// Builds the checkbox at the end of `parent`, a HorizontalBox. False when a widget would not spawn.
bool Build(void* parent);

// The widgets died with their menu.
void Forget();

// Paints the box from the state: a screen just shown.
void Sync();

// The pointer's look, on the menu tick.
void UpdateHover();

// A released click: true when it was this control's, and `changed` when that flipped the state.
bool OnReleaseEdge(bool& changed);

// The whole control, what a click lands on. Null before Build.
void* Box();

}  // namespace ui::server_browser_filter
