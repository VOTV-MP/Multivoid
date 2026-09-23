// ui/server_browser_tabs.h -- THE MASTER TABS above the server list: one per master slot
// (coop/net/master_slots.h), the chosen one filled. Picking a tab chooses whose list the browser
// shows and where a game the player hosts is listed, and the choice is remembered in the ini.
//
// MTA's browser keeps one tab per list SOURCE (CServerBrowser.cpp: a tab each for Internet, LAN,
// Favourites and Recent; OnTabChanged saves the options and fetches that source's list on its
// first view, or on every switch with auto-refresh on; the saved tab is restored when the browser
// is created). Divergence: the tabs re-feed the ONE list rather than each owning a list widget,
// because a lobby id is per master and the rows are the same shape on every tab; so a switch
// always fetches, since the rows it showed were the other master's. The skin is the list rows'
// own, the kit's selectable skin (native_screen.h): selection is the FILL, hover the frame and the
// TEXT colour, and a selected tab is never lit by the pointer. Hit-tested by geometry, since these
// are hand-built images. Game thread only.

#pragma once

namespace ui::server_browser_tabs {

// Build the strip into `parent`, the list's column, above the list. False if a widget could not
// be built: the caller fails the screen rather than showing a list with no way to change it.
bool Build(void* parent);

// The menu instance died and took the widgets with it.
void Forget();

// Repaint every tab (a screen just shown). A pointer move or a click repaints only the tabs whose
// look changed, by itself.
void Sync();

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
