// ui/server_browser_native.h -- the multiplayer server browser as a native UMG screen: a user
// widget built by hand (reflection only, no blueprint, no pak), added as a child of the menu's
// widget switcher and shown by writing the switcher's active index, how the game shows its own
// sub-screens. The measured facts it rests on: a never-initialised user widget renders inside
// the live switcher; the subtree survives a forced garbage collection through the switcher's
// own properties, so rooting is not used; Slate's hover reads false inside a scroll box while
// the cursor is within the rect, so rows hit-test by geometry and only the chrome outside the
// list asks Slate; a tinted image with no resource draws a solid rect, the game's own dimming
// scrim; and the switcher fills the screen above the menu chrome, so we paint over it and
// hit-test first while the game's cursor still draws above us. The list (rows, id pairing,
// hover, selection) lives in ui/server_browser_rows.h; this file owns the window: the scrim,
// the frame, the title strip, the footer, the switcher lifecycle and the escape and
// chrome-click polls. Hover is evaluated a tick after the pointer moved, since the
// window-procedure detour may only set a flag. Everything is game thread, driven from the
// menu's tick observer, except Open, a deferred intent.

#pragma once

#include "coop/net/lobby_client.h"   // LobbyRow -- what SelectedRow hands back

#include <cstdint>

namespace ui::server_browser_native {

// Ask for the browser. Safe from any thread and at any time, including from the harness
// mid-travel before a menu exists: it records a want-open intent that the menu tick consumes
// on a main-menu tick once the screen is built, and the intent expires if that never happens,
// so a join aborted into a world that never returns to the menu cannot leave a browser armed
// to pop up two sessions later.
void Open();

// Hide the screen (restoring the switcher's previous index if it is still ours) and drop any
// pending intent. Safe from any thread.
void Close();

// Close it right now, on the game thread, restoring the switcher index before returning. The
// deferred close is wrong for one caller, opening a sibling screen: both are children of one
// switcher, and the hosting window records the index it replaced so its back can restore it.
// If the browser is still the active child when the window opens, which a deferred close
// guarantees since the window's own tick runs later in the same menu tick, the window records
// the browser's index, the browser's later hide sees the index is no longer its own and skips
// the restore, and back lands the player on a browser whose shown flag is false: it paints,
// and every key and click it owns is dead, stranded until a level travel. The self-check could
// not see it, since it asserts the window up and the browser closed, both true while the
// chain was corrupt.
void CloseNow();

// True while our screen is the switcher's active child. Reconciled against the live index
// every tick, so a sibling screen navigating away is observed rather than assumed.
bool IsOpen();

// Which row the pointer is on, and which row is chosen; -1 or empty when none. Read-only, and
// it exists because those two facts had no observer outside the pixels: hover was silently
// dead for as long as the screen existed (the walk was gated on Slate's hover of the list,
// which reads false while the cursor is genuinely over it), and nothing could see it, because
// a highlight that never appears looks exactly like a cursor that was never there. The
// self-check asserts on these, and the connect control reads the selected id as its input.
int HoveredRow();
const char* SelectedRowId();

// The chosen row's data, false when nothing is chosen. Resolved by lobby id against the rows
// this screen last rendered, never by index: the client sorts, so the order is stable for a
// given set, but the set churns and one host leaving shifts every row after it, and an index
// would silently connect a player to a different server than the one they clicked; the full
// invariant lives with the code that keeps it, in ui/server_browser_rows.h. A selection whose
// lobby has since vanished answers false.
bool SelectedRow(coop::net::lobby::LobbyRow& out);

// A sentence for the footer, shown now and held against the next list sync. The footer
// normally mirrors the session manager's status, which the periodic sync rewrites, so a
// message produced by a click would either not appear until the next sync or be wiped by it;
// this writes immediately and suppresses that overwrite while fresh, which is what lets a
// button answer the player at all.
void SetNotice(const char* text);

// Why row `i` did not hover: dump its parts with their live visibility and hover state.
// Diagnostic, dev path only, and it reports a signal production no longer consumes: each
// part's Slate hover, which the walk asked before it moved to geometry. Deliberate: the
// question it answers is whether Slate agrees with the rects, and the answer being no inside a
// scroll box is the measurement the row model rests on. Read it as a comparison against
// geometry, not as what the screen uses.
void LogRowHitDiagnostics(int32_t i);

// Called from the multiplayer menu's tick post-observer, main menu only. `menu` is the live
// menu widget and `switcher` its widget switcher (null means no-op and retry next tick).
// Builds once per menu instance, fail-closed on donors.
void OnMenuTick(void* menu, void* switcher);


}  // namespace ui::server_browser_native
