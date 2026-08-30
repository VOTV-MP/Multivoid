// ui/host_window_native.h -- "Host game" as a NATIVE (UMG) screen.
//
// WHAT THIS IS. The second screen built on `ui/native_screen`'s kit and the sibling of
// `server_browser_native`: a hand-built `UUserWidget` added to `ui_menu_C::switcher_widgets`
// and shown by writing that switcher's ActiveWidgetIndex. Read
// `ui/server_browser_native.h` first -- every measured fact about how such a screen is
// built, survives GC, hit-tests and paints lives there and is not repeated.
//
// WHY IT EXISTS (user, 2026-08-29): *"добавим окно хостинга чтоб выбирать тип сервера ...
// чтобы не связываться с мастер сервером, когда это не требуется"*. The CHOICE already
// exists and already works -- `host_save_picker` has offered AUTO / DIRECT / LAN only
// since `8238539e` -- but only on the ImGui surface. The native browser has no Host at
// all, so finishing that lane means bringing the choice with it.
//
// ONE ACTION, TWO VIEWS. This window authors NO hosting logic. It collects a
// `session_manager::SaveChoice` plus the connection mode and calls the same
// `session_manager::HostWithSave(...)` the ImGui picker calls. A second host
// implementation would be the RULE-2 violation this note exists to forbid; if a rule
// about hosting needs to change, it changes in `session_manager`, once.
//
// THE CONNECTION MODES, and what each one actually costs the player (the wording is the
// product surface, so it is fixed here rather than invented at each call site):
//
//   AUTO       master-brokered ICE; direct when the NAT allows, TURN relay automatically
//              when it does not. ALWAYS LISTED -- for a relayed game the master is the
//              only rendezvous, so a hidden AUTO game is unjoinable.
//   DIRECT     a LanDirect UDP listen on net.port, which the host has forwarded. The
//              announce carries conn=direct + the port and the master publishes the
//              announce's source ip. Optionally unlisted: friends still Direct Connect.
//   LAN ONLY   never contacts the master at ALL -- no announce, no heartbeat, no
//              signaling; nothing leaves the machine -- and the accept edge refuses
//              non-private remote addresses.
//
// NO TEXT ENTRY IN v1, DELIBERATELY. The kit can build panels, images, text blocks and
// real buttons; it cannot yet build a focusable `UEditableTextBox`, and whether a
// hand-built one takes keyboard focus inside a never-`Initialize()`d widget tree is
// UNMEASURED (its offsets are already in sdk_profile.h, so the question is focus, not
// layout). Rather than block the window on that, v1 hosts under the same default name the
// ImGui surface uses and leaves renaming to that surface. The measurement, and then either
// a native box or our own WndProc-driven field, is the next step -- `WndProcDetour` runs
// on the GAME THREAD (measured 2026-07-31), which is what makes the second option cheap.
//
// FAILURE IS THE POINT, NOT AN EDGE CASE. The user's report that opened this lane was
// *"nothing told about the session being DEAD"*. `session_manager::HostStatus()` already
// carries the answer and its only reader is `server_browser.cpp` -- a window the user has
// just left. This screen therefore shows the status where the action was taken, and does
// not close itself on a failed host.

#pragma once

namespace ui::host_window_native {

// Ask for the window. Safe from any thread; the intent is consumed on a MAIN-menu tick and
// EXPIRES if no such tick arrives, exactly like the browser's.
void Open();

// Hide it (restoring the switcher's previous index if it is still ours) and drop any
// pending intent. Safe from any thread.
void Close();

// True while our screen is the switcher's active child, reconciled against the live index
// every tick rather than assumed.
bool IsOpen();

// The window's own X, for the selftest to aim at. A seam rather than a guess at its
// screen position: nothing had ever driven this button, and on 2026-08-30 a player found
// out by hand that they could not leave the window with it.
void* CloseButton();

// Called from coop::multiplayer_menu's ui_menu_C::Tick post-observer, MAIN menu only.
// Builds once per menu instance, fail-closed on donors.
// WHICH WORLD IS CHOSEN (-1 = New game), HOW MANY there are to choose from, and the list
// widget itself -- read-only, for the self-check to aim at and assert on.
//
// The save rows are `UImage`s inside a `UScrollBox`, which is the construct measured dead on
// 2026-08-29: `IsHovered()` reads 0 on one even when its rect contains the cursor. This
// window shipped with that hit test, so `SelectedSave()` could only ever be -1 -- the world
// list was decoration and HOST could only ever start a New game. It was cured by moving to
// geometry, and a cure that shares a mechanism with something that passed is still not a
// measurement, which is what these exist to make possible.
int   SelectedSave();
int   SaveRowCount();
void* SaveListWidget();

void OnMenuTick(void* menu, void* switcher);

}  // namespace ui::host_window_native
