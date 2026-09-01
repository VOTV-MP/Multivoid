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
// IT NO LONGER HOSTS ANYTHING, AND THAT IS THE POINT (2026-08-31). This window is step
// ONE of two: it settles WHICH WORLD and HOW TO CONNECT, and its confirm button is
// **Next**, which hands both to `ui/host_session_settings` -- step two, where WHO MAY JOIN
// is settled and the single `HostWithSave` call is made. The user asked for exactly that
// split: *"когда он выбрал сейв или new game, далее юзеру покажем еще одно окно, где уже
// будет настройка сессии, пароль замок и тд ... и только после этого он может уже хост
// кнопку нажать."*
//
// So the RULE-2 note this paragraph used to carry is now stronger, not weaker: there is
// still exactly ONE host action in the tree, and it is no longer here. A second one -- a
// "quick host" door that skips step two -- would be the violation. If a rule about hosting
// needs to change, it changes in `session_manager`, once.
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
// There were THREE until 2026-09-01. "LAN ONLY" is retired: it bound the SAME
// all-interfaces socket as DIRECT (`[V]` `:::47621`), and what made it look separate was an
// accept edge refusing non-private remotes -- deleted, because it did the router's job --
// plus never announcing, which is DIRECT + Hidden. THE ACCEPT EDGE NO LONGER REFUSES
// ANYTHING; this list said it did for one commit, which is a deleted security control
// described in the present tense. See coop/session/host_mode.h.
//
// NO TEXT ENTRY HERE, AND THE REASON IS NO LONGER THE ONE THIS NOTE USED TO GIVE. It said
// the kit "cannot yet build a focusable `UEditableTextBox`" and that focus was UNMEASURED.
// It has been measured since (2026-08-30, `coop/dev/native_text_probe`): Slate does not
// route keystrokes into a hand-wired, never-`Initialize()`d tree at all, so the answer was
// no, and `ui/native_text_field` owns its input at the WndProc seam instead. Two shipped
// screens use it.
//
// This window still does not name the world, because nothing here is a name: the session's
// NAME is derived from the player's nick (`<nick>'s game`), which is what tells another
// player in the browser who is hosting. Renaming is `browser_input_screens`' Change-name
// window, one door away, and it changes the nick -- one value, one owner.
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

// THE SAME CLOSE, PERFORMED NOW -- for a sibling screen that is about to take our place in
// the switcher. GAME THREAD ONLY; off-thread it degrades to the deferred `Close()`.
//
// A sibling MUST call this before showing itself, and the ordering is the whole
// correctness of the hand-over rather than a nicety: both screens are children of one
// switcher, and each records the index it replaces so its own Back can restore it. Open a
// sibling on top of a live one and the sibling records OUR index -- so its Back returns the
// player to a window that has already reconciled itself closed and is no longer listening.
// The browser learned this first (`server_browser_native::CloseNow`); the hosting flow
// gained a second hop on 2026-08-31 and needed the same rule.
void CloseNow();

// True while our screen is the switcher's active child, reconciled against the live index
// every tick rather than assumed.
bool IsOpen();

// The window's Back button, for the selftest to aim at. A seam rather than a guess at its
// screen position.
//
// It used to be the X, and the X is GONE (USER 2026-08-30: "не надо крестиков значит.
// Пусть окна закрывает юзер также как и нативные менюшки votv" -- no native VOTV window
// has one). That makes this button and ESC the ONLY ways out, so what has to be driven is
// this, not the control we removed. Note the X was measured WORKING the same day it was
// deleted (`HOST X PASS`, 23:43) -- it went for fidelity, not because it was broken.
void* BackButton();

// ...and the button that ADVANCES, for the same reason. It used to say "Host" and to BE the
// host action; it is now the door to step two, so what a self-check must drive is whether
// that door opens -- a claim nothing could make before, because the only way into the
// second window would otherwise be a dev flag.
void* NextButton();

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
