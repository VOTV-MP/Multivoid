// ui/overlay_test_arm.cpp -- see ui/overlay_test_arm.h.
//
// Extracted 2026-08-28 from imgui_overlay.cpp::Init(). The nine env blocks
// below are MOVED VERBATIM (log strings included -- probes grep them); the only
// seam edits are the two writes that touched imgui_overlay TU-locals, now the
// publics imgui_overlay::SetVisible(true) (an identical store) and
// imgui_overlay::ForceScoreboardOpen() (the FORCED latch, documented at its
// declaration).

#include "ui/overlay_test_arm.h"

#include "ui/imgui_overlay.h"
#include "ui/server_browser.h"
#include "coop/session/join_progress.h"
#include "coop/session/session_manager.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <string>

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
    // VOTVCOOP_SCOREBOARD_OPEN=1 starts the player list visible (the smoke can't
    // hold/press the tilde key) -- autonomous screenshot of the roster.
    char sbEnv[8] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_SCOREBOARD_OPEN", sbEnv, sizeof(sbEnv)) > 0 &&
        sbEnv[0] == '1') {
        imgui_overlay::ForceScoreboardOpen();
        UE_LOGI("imgui_overlay: VOTVCOOP_SCOREBOARD_OPEN=1 -- scoreboard starts visible (screenshot test)");
    }
    // VOTVCOOP_BROWSER_OPEN=1 starts the MULTIPLAYER server browser visible (the
    // boot-to-menu screenshot can't click the injected button) -- autonomous proof.
    char brEnv[8] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_BROWSER_OPEN", brEnv, sizeof(brEnv)) > 0 &&
        brEnv[0] == '1') {
        ui::server_browser::Open();
        UE_LOGI("imgui_overlay: VOTVCOOP_BROWSER_OPEN=1 -- server browser starts visible (screenshot test)");
    }
    // VOTVCOOP_TEST_CONNECT_DIRECT=<host:port> autonomously simulates a browser
    // "Direct connect" click: queues a LanDirect client Config that the harness
    // RunPlayLoop consumes (TakePendingStart) + boots g_session -- proving the
    // browser->session boot path without a real click. TEST-ONLY (never set in play).
    char cdEnv[64] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_CONNECT_DIRECT", cdEnv, sizeof(cdEnv)) > 0 && cdEnv[0]) {
        coop::session_manager::ConnectDirect(cdEnv);
        UE_LOGI("imgui_overlay: VOTVCOOP_TEST_CONNECT_DIRECT=%s -- queued a browser-path session start (test)", cdEnv);
    }
    // VOTVCOOP_TEST_HOST_LOBBY=1 simulates a browser "Host Game" click: POST /v1/host
    // to the master -> P2P host session (the exact path DoHost() runs). TEST-ONLY.
    char hlEnv[8] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_HOST_LOBBY", hlEnv, sizeof(hlEnv)) > 0 && hlEnv[0] == '1') {
        coop::session_manager::HostLobby("Test Host", std::string(), /*locked=*/false, /*playersMax=*/4);
        UE_LOGI("imgui_overlay: VOTVCOOP_TEST_HOST_LOBBY=1 -- fired a browser-path HOST announce (test)");
    }
    // VOTVCOOP_TEST_JOIN_LOBBY=<lobbyId> simulates clicking a browser row's Connect:
    // POST /v1/join -> P2P client session (the exact path JoinLobby() runs). TEST-ONLY.
    char jlEnv[64] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_JOIN_LOBBY", jlEnv, sizeof(jlEnv)) > 0 && jlEnv[0]) {
        coop::session_manager::JoinLobby(jlEnv, jlEnv);  // lobbyId doubles as the display label in the test
        UE_LOGI("imgui_overlay: VOTVCOOP_TEST_JOIN_LOBBY=%s -- fired a browser-path JOIN (test)", jlEnv);
    }
    // VOTVCOOP_TEST_HOST_SAVE=<slot> simulates the Host-Game picker's "Host selected
    // save": HostWithSave({existing slot}) -> the harness LOADS <slot> then hosts (the
    // exact path the picker's DoHostExisting runs). VOTVCOOP_TEST_HOST_NEW=<name> is the
    // "New Game & Host" (story) path. TEST-ONLY (never set in play).
    char hsEnv[64] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_HOST_SAVE", hsEnv, sizeof(hsEnv)) > 0 && hsEnv[0]) {
        coop::session_manager::SaveChoice c;
        c.newGame = false;
        c.slot = hsEnv;
        coop::session_manager::HostWithSave(c, "Test Host", /*locked=*/false, /*playersMax=*/4);
        UE_LOGI("imgui_overlay: VOTVCOOP_TEST_HOST_SAVE=%s -- fired a picker HOST-WITH-SAVE (load existing, test)", hsEnv);
    }
    char hnEnv[64] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_HOST_NEW", hnEnv, sizeof(hnEnv)) > 0 && hnEnv[0]) {
        coop::session_manager::SaveChoice c;
        c.newGame = true;
        c.newName = hnEnv;
        c.mode = 0;  // story
        coop::session_manager::HostWithSave(c, "Test Host", /*locked=*/false, /*playersMax=*/4);
        UE_LOGI("imgui_overlay: VOTVCOOP_TEST_HOST_NEW=%s -- fired a picker HOST-WITH-SAVE (new story game, test)", hnEnv);
    }
    // VOTVCOOP_TEST_LOADING=1 forces the CLIENT connecting/loading state up (no real connect)
    // so the loading screen + the menu fade + the console can be screenshotted determin-
    // istically. Sets a partial determinate bar (1400/2313). TEST-ONLY -- the 90 s failsafe
    // (join_progress::MaybeTimeout) lifts it on its own.
    char ldEnv[8] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_LOADING", ldEnv, sizeof(ldEnv)) > 0 && ldEnv[0] == '1') {
        coop::join_progress::BeginConnect("Test Host");
        coop::join_progress::BeginSnapshot(2313);
        for (int i = 0; i < 1400; ++i) coop::join_progress::NotePropApplied();
        UE_LOGI("imgui_overlay: VOTVCOOP_TEST_LOADING=1 -- forced the loading state (1400/2313) for a screenshot (test)");
    }
}

}  // namespace ui::overlay_test_arm
