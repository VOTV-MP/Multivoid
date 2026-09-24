// ui/overlay_test_arm.h -- TEST-ONLY env-var arming of the overlay's own surfaces, for the screenshots
// an autonomous run cannot key: VOTVCOOP_MENU_OPEN=1 (the F1 menu), MENU_TAB=<Category>/<Pane> (the menu
// on one pane), SCOREBOARD_OPEN=1 (the player list) and TEST_LOADING=1 (the client's connecting state
// with a partial bar, which the session loop reads too). Each is inert unless set, so a normal player
// boot does nothing here. The variables that do what a click in the multiplayer menu does are
// harness/browser_click_arm's.
//
// WHY IT LIVES HERE AND NOT IN THE OVERLAY: imgui_overlay.cpp owns the DXGI hooks, the WndProc and
// surface compositing. These blocks are the harness's ACTUATION points about those surfaces --
// ui/overlay_diag.cpp is the same argument for the observation side -- and they had pushed that
// file past the 800-line soft cap.

#pragma once

namespace ui::overlay_test_arm {

// Read the four variables once and arm what they name.
// Called from imgui_overlay::Init() after g_installed goes true.
void ArmFromEnv();

}  // namespace ui::overlay_test_arm
