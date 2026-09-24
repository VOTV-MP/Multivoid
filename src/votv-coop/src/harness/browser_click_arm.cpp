// harness/browser_click_arm.cpp -- see harness/browser_click_arm.h. Runs wait on the substrings
// "server browser starts visible" and "queued a browser-path session start" of the lines below.

#include "harness/browser_click_arm.h"

#include "ui/server_browser_surface.h"  // which browser this session uses
#include "coop/net/master_slots.h"
#include "coop/session/session_manager.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <string>

namespace harness::browser_click_arm {

void FireFromEnv() {
    // Win32 env reads (no CRT getenv -- /W4-clean in a DLL). Each call below only records a request or
    // starts a worker, so none of them needs the game thread.
    char brEnv[8] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_BROWSER_OPEN", brEnv, sizeof(brEnv)) > 0 && brEnv[0] == '1') {
        ui::server_browser_surface::Open();
        UE_LOGI("harness: VOTVCOOP_BROWSER_OPEN=1 -- server browser starts visible (screenshot test)");
    }
    // A LanDirect client Config the play loop consumes (TakePendingStart).
    char cdEnv[64] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_CONNECT_DIRECT", cdEnv, sizeof(cdEnv)) > 0 && cdEnv[0]) {
        coop::session_manager::ConnectDirect(cdEnv);
        UE_LOGI("harness: VOTVCOOP_TEST_CONNECT_DIRECT=%s -- queued a browser-path session start (test)", cdEnv);
    }
    // POST /v1/host to the chosen master, then a P2P host session in whatever world is loaded.
    char hlEnv[8] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_HOST_LOBBY", hlEnv, sizeof(hlEnv)) > 0 && hlEnv[0] == '1') {
        coop::session_manager::HostLobby("Test Host", std::string(), /*locked=*/false, /*playersMax=*/4);
        UE_LOGI("harness: VOTVCOOP_TEST_HOST_LOBBY=1 -- fired the test-only HostLobby announce (test)");
    }
    // POST /v1/join to the chosen master, then a P2P client session. The lobby id doubles as the display
    // label; with no row there is no host version to check first.
    char jlEnv[64] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_JOIN_LOBBY", jlEnv, sizeof(jlEnv)) > 0 && jlEnv[0]) {
        coop::session_manager::JoinLobby(coop::net::master_slots::Selected().url, jlEnv, jlEnv);
        UE_LOGI("harness: VOTVCOOP_TEST_JOIN_LOBBY=%s -- fired a browser-path JOIN (test)", jlEnv);
    }
    // HostWithSave: the harness loads the save (or starts the new game), then hosts and announces to the
    // chosen master.
    char hsEnv[64] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_HOST_SAVE", hsEnv, sizeof(hsEnv)) > 0 && hsEnv[0]) {
        coop::session_manager::SaveChoice c;
        c.newGame = false;
        c.slot = hsEnv;
        coop::session_manager::HostWithSave(c, "Test Host", /*locked=*/false, /*password=*/"", /*playersMax=*/4,
            coop::net::master_slots::Selected().url);
        UE_LOGI("harness: VOTVCOOP_TEST_HOST_SAVE=%s -- fired a picker HOST-WITH-SAVE (load existing, test)", hsEnv);
    }
    char hnEnv[64] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_HOST_NEW", hnEnv, sizeof(hnEnv)) > 0 && hnEnv[0]) {
        coop::session_manager::SaveChoice c;
        c.newGame = true;
        c.newName = hnEnv;
        c.mode = 0;  // story
        coop::session_manager::HostWithSave(c, "Test Host", /*locked=*/false, /*password=*/"", /*playersMax=*/4,
            coop::net::master_slots::Selected().url);
        UE_LOGI("harness: VOTVCOOP_TEST_HOST_NEW=%s -- fired a picker HOST-WITH-SAVE (new story game, test)", hnEnv);
    }
}

}  // namespace harness::browser_click_arm
