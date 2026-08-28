// ui/overlay_test_arm.h -- TEST-ONLY env-var arming of overlay surfaces and
// browser-path session scenarios: the mp.py/autonomous-harness entry seam into
// the UI, off the hot file.
//
// Nine VOTVCOOP_* variables, read once from imgui_overlay::Init() right after
// the present hook is installed. Every block is inert unless its variable is
// set, so a normal player boot does nothing here: MENU_OPEN / SCOREBOARD_OPEN /
// BROWSER_OPEN start a surface visible for screenshots the harness cannot key;
// TEST_CONNECT_DIRECT / TEST_HOST_LOBBY / TEST_JOIN_LOBBY / TEST_HOST_SAVE /
// TEST_HOST_NEW fire the exact session_manager paths the browser/picker clicks
// run; TEST_LOADING forces the client connecting/loading state for a
// deterministic loading-screen shot.
//
// WHY IT LIVES HERE AND NOT IN THE OVERLAY: imgui_overlay.cpp owns the DXGI
// hooks, the WndProc and surface compositing. These blocks are the autonomous
// harness's ACTUATION points about those surfaces -- ui/overlay_diag.cpp is the
// same argument for the observation side -- and they had pushed that file past
// the 800-LOC soft cap. Extracted 2026-08-28.

#pragma once

namespace ui::overlay_test_arm {

// Read the VOTVCOOP_* test variables once and arm whatever they name.
// Called from imgui_overlay::Init() after g_installed goes true.
void ArmFromEnv();

}  // namespace ui::overlay_test_arm
