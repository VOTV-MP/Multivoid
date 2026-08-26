// ui/server_browser_selftest.h -- the native browser's DEV self-check.
//
// WHAT THIS IS. A phase machine that drives the native server browser through the
// questions a screenshot cannot answer, and prints a MACHINE verdict for tools/mp.py to
// assert on. It runs only under [dev] browser_autoopen and ships dark; nothing in the
// player-facing path calls it.
//
// WHY ITS OWN TU. It is a distinct concept from the screen it inspects -- an instrument,
// not a renderer -- and it is the half that grows: every step from T0 onward in
// docs/MULTIPLAYER_UI.md section 8c.-1 adds a question here (T1 owes Close() a driven
// test shown RED first). server_browser_native.cpp was at 625 LOC with the check inside
// it, and T0 alone would have taken it to ~775 with no headroom left, so the extraction
// happens BEFORE the growth rather than after the cap is breached -- the trigger the
// modular rule actually describes.
//
// THE SEAM IS THREE WIDGET POINTERS, deliberately: the check needs the scrim, the list and
// the close button and nothing else, so the screen exposes no accessors and keeps no probe
// state. All three are passed per tick, so a rebuilt menu cannot leave this holding a dead
// pointer. (It does reach back for `IsOpen()`, which is already public and is the only
// question the pointers cannot answer.)
//
// GAME THREAD ONLY. Every call reaches the engine through ProcessEvent, and the input
// synthesis below assumes our window is foreground.

#pragma once

namespace ui::server_browser_selftest {

// Start the sequence from phase 0. Called once, when browser_autoopen shows the screen.
void Arm();

// One menu tick. The three are the browser's own widgets; any may be null, in which case
// the phases that need it report a SKIP rather than a failure. A no-op until Arm(), and a
// no-op again once the sequence has run.
//
// CALLED WHETHER OR NOT THE SCREEN IS SHOWN. The last phases close it with ESC, RE-OPEN it,
// and then drive the X -- which is impossible if the caller gates this on visibility.
void Tick(void* scrim, void* list, void* closeBtn);

}  // namespace ui::server_browser_selftest
