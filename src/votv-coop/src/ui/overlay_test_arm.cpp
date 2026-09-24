// ui/overlay_test_arm.cpp -- see ui/overlay_test_arm.h.
//
// The four env blocks below keep their log strings exactly as written, because runs wait on them. The
// two writes that reach imgui_overlay go through its publics rather than its TU-locals: SetVisible(true),
// and ForceScoreboardOpen() for the forced latch, documented at its declaration.

#include "ui/overlay_test_arm.h"

#include "ui/dev_menu.h"
#include "ui/imgui_overlay.h"
#include "coop/session/join_progress.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <cstring>

namespace ui::overlay_test_arm {

void ArmFromEnv() {
    // Autonomous screenshot test: VOTVCOOP_MENU_OPEN=1 starts the menu visible (the
    // smoke can't press F1). Win32 env read (no CRT getenv -- /W4-clean in a DLL).
    char menuEnv[8] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_MENU_OPEN", menuEnv, sizeof(menuEnv)) > 0 &&
        menuEnv[0] == '1') {
        imgui_overlay::SetVisible(true);
        UE_LOGI("imgui_overlay: VOTVCOOP_MENU_OPEN=1 -- menu starts visible (screenshot test)");
    }
    // VOTVCOOP_MENU_TAB=<Category>/<Pane> opens the menu on one pane ("World/Rules"): the smoke
    // cannot click a pane either.
    char tabEnv[64] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_MENU_TAB", tabEnv, sizeof(tabEnv)) > 0) {
        if (char* slash = std::strchr(tabEnv, '/')) {
            *slash = 0;
            ui::dev_menu::RequestSelect(tabEnv, slash + 1);
            UE_LOGI("imgui_overlay: VOTVCOOP_MENU_TAB=%s/%s -- menu opens on that pane (screenshot test)",
                    tabEnv, slash + 1);
        }
    }
    // VOTVCOOP_SCOREBOARD_OPEN=1 starts the player list visible (the smoke can't
    // hold/press the tilde key) -- autonomous screenshot of the roster.
    char sbEnv[8] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_SCOREBOARD_OPEN", sbEnv, sizeof(sbEnv)) > 0 &&
        sbEnv[0] == '1') {
        imgui_overlay::ForceScoreboardOpen();
        UE_LOGI("imgui_overlay: VOTVCOOP_SCOREBOARD_OPEN=1 -- scoreboard starts visible (screenshot test)");
    }
    // VOTVCOOP_TEST_LOADING=1 forces the CLIENT connecting/loading state up (no real connect)
    // so the loading screen + the menu fade + the console can be screenshotted determin-
    // istically. Sets a partial determinate bar (1400/2313). TEST-ONLY -- the 90 s failsafe
    // (join_progress::MaybeTimeout) lifts it on its own.
    char ldEnv[8] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_LOADING", ldEnv, sizeof(ldEnv)) > 0 && ldEnv[0] == '1') {
        coop::join_progress::BeginConnect("Test Host", coop::join_progress::Stage::Dialing);
        coop::join_progress::BeginSnapshot(2313);
        for (int i = 0; i < 1400; ++i) coop::join_progress::NotePropApplied();
        UE_LOGI("imgui_overlay: VOTVCOOP_TEST_LOADING=1 -- forced the loading state (1400/2313) for a screenshot (test)");
    }
}

}  // namespace ui::overlay_test_arm
