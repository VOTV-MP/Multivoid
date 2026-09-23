// ui/server_browser_tabs.h -- THE MASTER TABS above the server list: one per master slot
// (coop/net/master_slots.h), the chosen one filled. Picking a tab chooses whose list the browser
// shows and where a game the player hosts is listed, and the choice is remembered in the ini.
//
// MTA's browser keeps one tab per list SOURCE (CServerBrowser.cpp: a tab each for Internet, LAN,
// Favourites and Recent; OnTabChanged saves the options and refreshes that source; the saved tab
// is restored on open). Divergence: the tabs re-feed the ONE list rather than each owning a list
// widget, because a lobby id is per master and the rows are the same shape on every tab. The skin
// is the list rows' own two channels (docs/votv-ui-style.md, State): selection is the FILL, hover
// the frame and the TEXT colour, and a selected tab is never lit by the pointer. Hit-tested by
// geometry, since these are hand-built images. Game thread only.

#pragma once

namespace ui::server_browser_tabs {

// Build the strip into `parent`, the list's column, above the list. False if a widget could not
// be built: the caller fails the screen rather than showing a list with no way to change it.
bool Build(void* parent);

// The menu instance died and took the widgets with it.
void Forget();

// Repaint both channels if the selection or the pointer moved since the last paint; `force`
// repaints regardless (a screen just shown). Cheap otherwise: two integer compares.
void Sync(bool force);

// Re-evaluate the hover. Only when the pointer moved (plus one settling pass after): resolving the
// cursor into widget space reaches an object-array walk.
void UpdateHover();

// A left-button release on the browser: if it landed on a tab, that master becomes the chosen one
// and true comes back, with `switched` saying whether the list changed. The caller stops routing
// the click and, on a switch, refetches and repaints.
bool OnReleaseEdge(bool& switched);

// For the self-check to aim at: how many tabs there are, and tab `i`'s hit target.
int   Count();
void* Tab(int i);

}  // namespace ui::server_browser_tabs
