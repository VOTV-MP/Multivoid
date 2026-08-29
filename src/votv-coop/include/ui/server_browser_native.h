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
//   * `IsHovered()` ANSWERS on a bare `UImage` with Visibility=Visible: 1 with the cursor
//     inside its rect, 0 outside. That is the whole hit-test, and it is why rows carry no
//     UButton (the native row's own `button_select` draws nothing in all three states);
//   * a `UImage` with a TINT and NO ResourceObject draws a SOLID RECT -- the game's own
//     `ui_saveSlots_C.Image_302` is exactly that (full-screen, tint (0,0,0,0.5)) and it is
//     what dims the menu behind every native sub-screen. Our scrim copies it and needs no
//     donor at all;
//   * the switcher FILLS THE SCREEN (its CanvasPanelSlot has Anchors (0,0)-(1,1) and zero
//     offsets) and its container sits at index 8 of the `screen` canvas while the menu
//     chrome (`canvas_menu`) is index 5 -- so we paint ABOVE the chrome and Slate hit-tests
//     us first, while VOTV's own cursor image (index 12) still draws above us.
//
// TWO INVARIANTS THAT ARE EASY TO BREAK:
//
//  1. ROW IDENTITY IS THE `lobbyId`, NEVER AN INDEX INTO THE NETWORK LIST. The master
//     stores lobbies in a `HashMap` and emits `state.lobbies.values()` (master.rs:531-553),
//     and the client never sorts -- so one host leaving while another joins gives the SAME
//     COUNT in a DIFFERENT ORDER. A row therefore remembers the id it was RENDERED with,
//     captured in the same pass as its text, and a click resolves against THAT, never
//     against a fresher copy. `g_rowIds[i]` is positional only INSIDE this one structure,
//     which a single writer updates together with the child's text.
//
//  2. HOVER IS EVALUATED ON A LATER TICK THAN THE POINTER MOVED. Measured: sampling
//     IsHovered() in the same tick as the move reads the PREVIOUS position every time.
//     The WndProc detour runs BEFORE CallWindowProcW, so it may only set a moved-flag.
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
// this screen last RENDERED, never by index: the master stores lobbies in a HashMap and the
// client does not sort, so the same count can arrive in a different order and an index would
// silently connect a player to a different server than the one they clicked (invariant 1
// above). A selection whose lobby has since vanished from the list answers false.
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
// Diagnostic, dev-path only. A row is four widgets deep -- SizeBox, Overlay, the tinted
// background image, and a HorizontalBox of text blocks -- and exactly one of them wins
// Slate's hit test. Which one is not guessable from the code: the background is set
// Visible on purpose to BE the hit target, but anything painted over it that is also
// hit-testable takes the hit instead and leaves the background reading not-hovered, which
// is indistinguishable from the whole hit test being broken.
void LogRowHitDiagnostics(int32_t i);

// Called from coop::multiplayer_menu's ui_menu_C::Tick post-observer, MAIN menu only.
// `menu` is the live ui_menu_C; `switcher` its switcher_widgets (may be null -- then this
// no-ops and retries next tick). Builds once per menu instance, fail-closed on donors.
void OnMenuTick(void* menu, void* switcher);


}  // namespace ui::server_browser_native
