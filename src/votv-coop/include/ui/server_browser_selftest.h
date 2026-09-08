// ui/server_browser_selftest.h -- the native browser's dev self-check.
//
// A phase machine that drives the real browser screen through the questions a screenshot cannot
// answer -- scrolling, the scrim, ESC, the action bar, a row, Back, HOST and the windows behind
// it -- and prints a machine verdict for the test rig to assert on. It runs only under the dev
// browser auto-open and ships dark: nothing in the player-facing path calls it. It is its own
// translation unit because it is an instrument rather than a renderer, and because it is the
// half that grows: every new step is another question here.
//
// The screen hands it three widget pointers per tick (the scrim, the list, the Back button), so
// a rebuilt menu cannot leave it holding a dead one and the screen itself keeps no probe state;
// everything the pointers cannot answer it asks the browser, host-window, input-screen and
// session-settings modules directly. Game thread only, and the synthesized input assumes our
// window is foreground.

#pragma once

namespace ui::server_browser_selftest {

// Start the sequence from phase 0. Called once, when the dev auto-open shows the screen.
void Arm();

// One menu tick. The three are the browser's own widgets; any may be null, in which case the
// phases that need it report a SKIP rather than a failure. A no-op until Arm(), and a no-op
// again once the sequence has run.
//
// CALLED WHETHER OR NOT THE SCREEN IS SHOWN. Mid-ladder phases close the screen with ESC and
// re-open it before driving Back, which is impossible if the caller gates this on visibility.
void Tick(void* scrim, void* list, void* exitBtn);

}  // namespace ui::server_browser_selftest
