// harness/autotest.h -- Autonomous grab test (no user E-press required).
//
// Forced-grab routine: find the nearest Aprop_C derivative, teleport it
// to the player's hand, then drive grabHandle.GrabComponentAtLocation /
// SetTargetLocation / ReleaseComponent via reflection (these UFunctions
// are ProcessEvent-dispatched and observable -- so this routine exercises
// the FULL Stage-1 observer pipeline end-to-end without a real keypress).
//
// Gated by env VOTVCOOP_RUN_GRAB_TEST="1" + role=Host. Spawned from the
// netEnabled play loop on a dedicated thread; all engine work goes through
// GT::Post so we never touch UObject state off the game thread.

#pragma once

#include <windows.h>

namespace harness::autotest {

// Run the full autonomous grab test routine. Blocks the calling thread
// for ~20 seconds (waits + screenshot captures + multiple drive ticks).
// Designed to be called from a worker thread (see GrabTestThread).
void RunAutonomousGrabTest();

// Worker-thread wrapper: just calls RunAutonomousGrabTest then returns 0.
// Pass to ::CreateThread as the start routine.
DWORD WINAPI GrabTestThread(LPVOID arg);

// Phase 5F: autonomous flashlight-toggle test. Calls
// AmainPlayer_C::`Flashlight Update` via reflection 4 times with 2 s
// spacing. The POST observer detour catches each call + sends the
// ItemActivate wire packet. Both peers run this; the OTHER peer's
// puppet should reflect the toggles via the receiver path
// (item_activate::ApplyToPuppet).
// Blocks the calling thread for ~25 seconds (15 s settle + 4 * 2 s + tail).
// Gated by env VOTVCOOP_RUN_FLASHLIGHT_TEST="1".
void RunAutonomousFlashlightTest();
DWORD WINAPI FlashlightTestThread(LPVOID arg);

}  // namespace harness::autotest
