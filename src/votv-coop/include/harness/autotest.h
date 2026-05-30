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

// Phase 5W: autonomous weather sync test. Host-only. After stabilization,
// calls weather_sync::DebugForceRain to force rain on / off / on / off
// at 5-second intervals. The POST observer on setRainProperties catches
// each call and broadcasts a WeatherState packet. Verification:
//   - Host log: at least 2 `weather: host broadcast` lines.
//   - Client log: at least 2 `weather: applied flags 0x...` lines.
//   - Cross-peer state read (via a diagnostic log line every cycle on
//     BOTH peers): host `isRaining` and client `isRaining` should match
//     after each forced toggle.
// Blocks the calling thread for ~30 seconds.
// Gated by env VOTVCOOP_RUN_WEATHER_TEST="1" + role=Host.
void RunAutonomousWeatherTest();
DWORD WINAPI WeatherTestThread(LPVOID arg);

// Phase 5W Inc-fix-2 (2026-05-27): autonomous RED SKY sync test. Host-
// only sender. After stabilization, host calls
// weather_sync::DebugForceRedSky(true) to flip the entire scene's sky
// + ambient color curves to the red set. Host's POST observer on
// spawnRedSky catches + broadcasts; client invokes the same on its
// local gamemode. Verification: both peers' screenshots show red sky.
// Chosen over rain (user feedback 2026-05-27): rain particle rendering
// turned out to be mostly atmospheric mist, ambiguous in screenshots;
// red sky is unmistakable.
// Gated by env VOTVCOOP_RUN_REDSKY_TEST="1" + role=Host.
void RunAutonomousRedSkyTest();
DWORD WINAPI RedSkyTestThread(LPVOID arg);

// PR-FOUNDATION-2 (B): autonomous CLIENT save-block test. Client-only. After
// stabilization, resolves the live saveSlot_C world-save object and calls its
// saveToSlot UFunction via reflection -- driving UGameplayStatics::SaveGameToSlot
// (our client hook target) without needing an autosave/menu trigger. Proves the
// hook CANCELS the write (not just that it installed): the client log shows
// `saveblock_test: invoking ...` then `save_block: BLOCKED ...`. The host runs
// nothing and installs no hook (its save path is untouched).
// Gated by env VOTVCOOP_RUN_SAVEBLOCK_TEST="1".
void RunAutonomousSaveBlockTest();
DWORD WINAPI SaveBlockTestThread(LPVOID arg);

// PR-FOUNDATION-2 (B part 2): autonomous CLIENT Save-button grey-out test. Client-only.
// Drives mainPlayer_C::InpActEvt_Escape via reflection (a real ESC press is impossible
// autonomously) so coop/save_button_disable's POST observer fires and disables the live
// pause-menu button_Save; the module logs the GetIsEnabled read-back. Proves the disable
// took; the visual grey needs a human glance. Gated by env VOTVCOOP_RUN_SAVEBTN_TEST="1".
void RunAutonomousSaveBtnDisableTest();
DWORD WINAPI SaveBtnDisableTestThread(LPVOID arg);

}  // namespace harness::autotest
