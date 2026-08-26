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
// THE SEAM IS TWO WIDGET POINTERS, deliberately: the check needs the scrim and the list
// and nothing else, so the screen exposes no accessors and keeps no probe state. Both are
// passed per tick, so a rebuilt menu cannot leave this holding a dead pointer.
//
// GAME THREAD ONLY. Every call reaches the engine through ProcessEvent, and the input
// synthesis below assumes our window is foreground.

#pragma once

namespace ui::server_browser_selftest {

// Start the sequence from phase 0. Called once, when browser_autoopen shows the screen.
void Arm();

// One menu tick. `scrim` and `list` are the browser's own widgets; either may be null,
// in which case the phases that need it report a SKIP rather than a failure. A no-op
// until Arm(), and a no-op again once the sequence has run.
void Tick(void* scrim, void* list);

}  // namespace ui::server_browser_selftest
