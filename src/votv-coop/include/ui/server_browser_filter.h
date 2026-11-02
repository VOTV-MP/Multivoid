// ui/server_browser_filter.h -- the server browser's two checkboxes beside Back: "Hide incompatible" and "Hide locked".
//
// Checked, the first leaves out every lobby the join gate would refuse (ui/server_browser_rows owns that test) and the second
// every lobby that asks for a password; unchecked, they show with their red mark or padlock. Each choice is remembered in the
// ini (browser.hide_incompatible, browser.hide_locked), and the status pane says how many are hidden, so a filtered list never
// reads as an empty master. The state is here and any surface reads it; the widgets are the native browser's, spawned from the
// kit, as the game's font has no check glyph.

#pragma once

namespace coop::net::lobby {
struct LobbyRow;
}

namespace ui::server_browser_filter {

enum class Filter { Incompatible, Locked };

// Whether the list leaves these lobbies out. Any thread.
bool Hides(Filter f);

// Sets it and remembers it in the ini. Repaints the box if it is built, so a game-thread call.
void SetHides(Filter f, bool hide);

// Whether a checked box leaves this lobby out: the one test both surfaces filter with.
bool Leaves(const coop::net::lobby::LobbyRow& r);

// Builds the checkboxes at the end of `parent`, a HorizontalBox. False when a widget would not spawn.
bool Build(void* parent);

// The widgets died with their menu.
void Forget();

// Paints the boxes from the state: a screen just shown.
void Sync();

// The pointer's look, on the menu tick.
void UpdateHover();

// A released click: true when it was one of these controls', and `changed` when that flipped its state.
bool OnReleaseEdge(bool& changed);

// One whole control, what a click lands on. Null before Build.
void* Box(Filter f);

}  // namespace ui::server_browser_filter
