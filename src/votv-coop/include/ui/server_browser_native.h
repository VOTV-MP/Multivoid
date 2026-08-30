// ui/server_browser_native.h -- the MULTIPLAYER server browser as a NATIVE UMG screen.
//
// Design of record: docs/MULTIPLAYER_UI.md sections 7b / 7c / 8, with section 8a as the
// AS-BUILT measurement box. Read 8a before 8: it corrects three of section 8's statements.
//
// WHAT THIS IS. A `UUserWidget` we build by hand -- reflection only, no Blueprint, no
// editor, no pak -- added as the 12th child of `ui_menu_C::switcher_widgets` and shown by
// writing that switcher's ActiveWidgetIndex, which is exactly how the game shows its own
// sub-screens. Data and actions are unchanged: coop::net::lobby::LobbyRow in,
// coop::session_manager out. Only the renderer is native.
//
// THE FACTS IT RESTS ON, all measured (coop/dev/native_ui_probe, 2026-08-25/26):
//   * a never-`Initialize()`d UUserWidget RENDERS inside the live UWidgetSwitcher --
//     AddChild 11 -> 12, GetDesiredSize (0,0) -> (654,64), screenshot confirms;
//   * the subtree SURVIVES a forced GC. It is reachable purely through UPROPERTYs from
//     the live switcher (UUserWidget::WidgetTree -> UWidgetTree::RootWidget ->
//     UPanelWidget::Slots -> UPanelSlot::Content), so `AddToRoot` is NOT used here and
//     would be wrong -- it would outlive the menu and demand a paired un-root;
//   * `IsHovered()` answers on a bare `UImage` with Visibility=Visible AT THE ROOT OF THE
//     TREE -- 1 with the cursor inside its rect, 0 outside. **SCOPE CORRECTED 2026-08-30,
//     and the uncorrected version of this line cost the screen its row selection for as
//     long as it existed: inside a `UScrollBox` the same widget answers 0 while the cursor
//     is provably within its rect.** Measured -- row bg (796,540) 955x64 against a cursor
//     at (1280,572), every parent link Visible or SelfHitTestInvisible, IsHovered = 0,
//     with the X (a UButton outside the ScrollBox) reading 1 in the same tick. So this is
//     NOT the whole hit-test: rows hit-test by GEOMETRY through
//     `native_screen::ChildAtCursor`, and only chrome outside the list still asks Slate.
//     Rows still carry no UButton, for the separate reason that the native row's own
//     `button_select` draws nothing in all three states;
//   * a `UImage` with a TINT and NO ResourceObject draws a SOLID RECT -- the game's own
//     `ui_saveSlots_C.Image_302` is exactly that (full-screen, tint (0,0,0,0.5)) and it is
//     what dims the menu behind every native sub-screen. Our scrim copies it and needs no
//     donor at all;
//   * the switcher FILLS THE SCREEN (its CanvasPanelSlot has Anchors (0,0)-(1,1) and zero
//     offsets) and its container sits at index 8 of the `screen` canvas while the menu
//     chrome (`canvas_menu`) is index 5 -- so we paint ABOVE the chrome and Slate hit-tests
//     us first, while VOTV's own cursor image (index 12) still draws above us.
//
// WHAT THIS FILE NO LONGER OWNS (2026-08-30). The LIST -- the row widgets, the network rows
// behind them, the lobbyId pairing, hover and selection -- moved to
// `ui/server_browser_rows.h`, and the invariants that govern it moved WITH it rather than
// being restated here. Two headers describing one concept is how they drift. This file owns
// the WINDOW: the scrim, the frame, the title strip, the footer, the switcher lifecycle, and
// the ESC and chrome-click polls. `HoveredRow` / `SelectedRowId` / `SelectedRow` below are
// forwards, kept so a caller holding this header need not learn a second one.
//
// THE INVARIANT THAT IS STILL THIS FILE'S: HOVER IS EVALUATED ON A LATER TICK THAN THE
// POINTER MOVED. Measured: sampling in the same tick as the move reads the PREVIOUS position
// every time. The WndProc detour runs BEFORE CallWindowProcW, so it may only set a flag --
// which is why `native_screen::HoverTracker` owes a settling tick after motion stops.
//
// Threading: everything here is GAME THREAD, driven from coop::multiplayer_menu's existing
// ui_menu_C::Tick observer -- the one hands-on-verified native inject we have. Open() is
// the exception: it may be called from anywhere (the harness reopens the browser on four
// failure-recovery paths) and is therefore a DEFERRED INTENT, not an action.

#pragma once

#include "coop/net/lobby_client.h"   // LobbyRow -- what SelectedRow hands back

#include <cstdint>

namespace ui::server_browser_native {

// Ask for the browser. Safe from any thread and at any time -- including from the harness
// mid-travel, before a menu exists. It records a want-open intent that OnMenuTick consumes
// on a MAIN-menu tick (isPause == false) once the screen is built; the intent EXPIRES if
// that never happens, so a join aborted into a world that never returns to the menu cannot
// leave a browser armed to pop up two sessions later.
void Open();

// Hide the screen (restores the switcher's previous index if it is still ours) and drop
// any pending intent. Safe from any thread.
void Close();

// Close it RIGHT NOW, on the game thread, restoring the switcher index before returning.
//
// `Close()` is a deferred intent and that is wrong for one caller: opening a SIBLING screen.
// Both screens are children of one switcher, and the hosting window records the index it
// replaced so its Back can restore it. If the browser is still the active child when the
// window opens -- which is what a deferred close guarantees, since the window's own tick
// runs three lines later in the SAME menu tick -- the window records the BROWSER's index,
// the browser's later Hide sees the index is no longer its own and skips the restore, and
// Back then lands the player on a browser whose `g_shown` is false: it paints, and every
// key and click it owns is dead. Stranded until a level travel.
//
// Measured 2026-08-29 from the log line `host_window_native: shown (index 11 -> 12)` -- 11
// is the browser. The self-check could not see it: it asserts the window is up and the
// browser is closed, and both were true while the chain was corrupt.
void CloseNow();

// True while our screen is the switcher's active child. Reconciled against the live index
// every tick, so a sibling screen navigating away is observed rather than assumed.
bool IsOpen();

// WHICH ROW THE POINTER IS ON, and WHICH ROW IS CHOSEN. -1 / empty when there is none.
//
// Read-only, and it exists because those two facts had no observer outside the pixels.
// Hover was silently dead for as long as the screen has existed -- the walk was gated on
// `IsHovered(g_list)`, which reads false while the cursor is genuinely over the list -- and
// nothing could see it, because a highlight that never appears looks exactly like a cursor
// that was never there. The self-check asserts on these; the Connect control will read
// SelectedRowId() as its input.
int HoveredRow();
const char* SelectedRowId();

// The chosen row's DATA, false when nothing is chosen. Resolved by lobbyId against the rows
// this screen last RENDERED, never by index: the client sorts, so the order is stable for a
// given SET, but the set churns and one host leaving shifts every row after it -- an index
// would silently connect a player to a different server than the one they clicked. The full
// statement of that invariant lives with the code that keeps it, in
// `ui/server_browser_rows.h`. A selection whose lobby has since vanished answers false.
bool SelectedRow(coop::net::lobby::LobbyRow& out);

// A sentence for the footer, shown NOW and held against the next list sync.
//
// The footer normally mirrors `session_manager::Status()`, which the 5 s sync rewrites -- so
// a message produced by a click would either not appear until the next sync or be wiped by
// it. This writes immediately and suppresses that overwrite while it is fresh, which is what
// makes a button able to answer the player at all.
void SetNotice(const char* text);

// WHY ROW `i` DID NOT HOVER: dump its parts with their live visibility and hover state.
//
// Diagnostic, dev-path only, and it now reports a signal PRODUCTION NO LONGER CONSUMES:
// it prints each part's `IsHovered`, which is what the hover walk used to ask before the
// walk moved to geometry. That is deliberate -- the question it answers is "does Slate
// agree with the rects", and the answer being NO inside a ScrollBox is the measurement the
// whole row model now rests on. Read it as a comparison against geometry, not as the thing
// the screen uses.
void LogRowHitDiagnostics(int32_t i);

// Called from coop::multiplayer_menu's ui_menu_C::Tick post-observer, MAIN menu only.
// `menu` is the live ui_menu_C; `switcher` its switcher_widgets (may be null -- then this
// no-ops and retries next tick). Builds once per menu instance, fail-closed on donors.
void OnMenuTick(void* menu, void* switcher);


}  // namespace ui::server_browser_native
