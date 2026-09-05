// ui/host_window_native.h -- the host-game screen as a native UMG screen: the second screen
// built on the native-screen kit and the sibling of server_browser_native, a hand-built user
// widget added to the menu's widget switcher and shown by writing its active index; read
// ui/server_browser_native.h first, where every measured fact about building, GC survival,
// hit-testing and painting such a screen lives. This window hosts nothing: it is step one
// of two, settling which world and how to connect, and its Next button hands both to
// ui/host_session_settings, step two, where who may join is settled and the single host
// call is made; a quick-host door that skips step two would be the violation. The
// connection modes (the wording is the product surface): AUTO is master-brokered ICE,
// direct when the NAT allows and a relay when it does not, always listed since for a
// relayed game the master is the only rendezvous; DIRECT is a listen on the configured
// port the host has forwarded, the announce carrying the port and the master publishing the
// source address, optionally unlisted; see coop/session/host_mode.h. No text entry here (the
// native text field owns input at the window-procedure seam), and no name: the session's
// name derives from the player's nick. The status shows here; a failed host stays open.

#pragma once

namespace ui::host_window_native {

// Ask for the window. Safe from any thread; the intent is consumed on a main-menu tick and
// expires if no such tick arrives, exactly like the browser's.
void Open();

// Hide it (restoring the switcher's previous index if it is still ours) and drop any pending
// intent. Safe from any thread.
void Close();

// The same close, performed now, for a sibling screen about to take our place in the
// switcher. Game thread only; off-thread it degrades to the deferred close. A sibling must
// call this before showing itself, and the ordering is the whole correctness of the
// hand-over: both screens are children of one switcher, and each records the index it
// replaces so its own Back can restore it. Open a sibling on top of a live one and the
// sibling records our index, so its Back returns the player to a window that has already
// reconciled itself closed and is no longer listening.
void CloseNow();

// True while our screen is the switcher's active child, reconciled against the live index
// every tick rather than assumed.
bool IsOpen();

// The window's Back button, for the selftest to aim at: a seam rather than a guess at its
// screen position. There is no close cross (no native game window has one), so this button
// and Escape are the only ways out, and what has to be driven is this.
void* BackButton();

// The button that advances, for the same reason. It is the door to step two, so what a
// self-check must drive is whether that door opens.
void* NextButton();

// Called from the multiplayer menu's tick post-observer, main menu only; builds once per menu
// instance, fail-closed on donors. Which world is chosen (-1 is New game), how many there
// are, and the list widget itself: read-only, for the self-check to aim at and assert on.
// The save rows are images inside a scroll box, where Slate's hover reads false even when
// the rect contains the cursor, so the hit test is by geometry; these exist so that is
// measured rather than assumed.
int   SelectedSave();
int   SaveRowCount();
void* SaveListWidget();

void OnMenuTick(void* menu, void* switcher);

}  // namespace ui::host_window_native
