// coop/dev/menu_proceed.cpp -- see coop/dev/menu_proceed.h. TEST-ONLY.
//
// Reaching VOTV's MAIN MENU autonomously (for the MULTIPLAYER-button screenshot) is
// blocked at boot by the OMEGA content-warning screen: it is a special early-boot
// widget (NOT ui_menu_C -- proven: FindObjectByClass("ui_menu_C")==null there) with
// no standard UButtons (a full GUObjectArray scan found 0 live bound UButtons), and
// VOTV's menu cursor is raw-input driven so synthetic OS clicks don't activate it.
//
// So instead of fighting OMEGA, we go the OTHER way: wait until the game reaches
// GAMEPLAY (which the harness boots past OMEGA via StartFreshGame/LoadStorySave),
// then issue VOTV's own AmainGamemode_C::transition("/Game/menu"). Quitting to the
// menu loads the REAL ui_menu_C main menu WITHOUT re-showing OMEGA, and our
// ui_menu_C::Tick observer (coop::multiplayer_menu) injects the MULTIPLAYER button.
// A transparent ProcessEvent bypass covers the dying-world teardown (our detour
// otherwise hangs the untitled_1 EndPlay storm -- see autotest_menutravel_probe);
// it auto-expires so observers re-engage at the loaded menu.
//
// Requires the launch to boot gameplay (scenario=play, ideally fresh_boot=1).

#include "coop/dev/menu_proceed.h"

#include "ui/multiplayer_menu.h"
#include "coop/session/shutdown.h"
#include "ue_wrap/call.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace coop::dev::menu_proceed {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace prof = ue_wrap::profile;
using ue_wrap::Call;
using ue_wrap::ParamFrame;

namespace {

std::atomic<bool> g_running{false};

// Read the live UWorld leaf name on the game thread ("<null>" if none).
std::wstring WorldNameGT() {
    auto done = std::make_shared<std::atomic<int>>(0);
    auto out = std::make_shared<std::wstring>(L"<null>");
    GT::Post([done, out] {
        if (void* w = R::FindObjectByClass(prof::name::WorldClass)) *out = R::ToString(R::NameOf(w));
        done->store(1);
    });
    for (int i = 0; i < 1600 && done->load() == 0; ++i) ::Sleep(5);
    return *out;
}

bool InGameplay(const std::wstring& world) { return world.find(L"ntitled") != std::wstring::npos; }

// AmainGamemode_C::transition(FName LevelName) -- VOTV's own level travel. Game thread.
bool CallTransition(const std::wstring& level) {
    void* gm = R::FindObjectByClass(prof::name::GamemodeClass);
    if (!gm || !R::IsLive(gm)) { UE_LOGW("menu_proceed: no live mainGamemode_C"); return false; }
    void* fn = R::FindFunction(R::ClassOf(gm), L"transition");
    if (!fn) { UE_LOGW("menu_proceed: transition() not resolved"); return false; }
    R::FName ln = ue_wrap::fname_utils::StringToFName(level);
    ParamFrame f(fn);
    if (!f.valid() || !f.SetRaw(L"LevelName", &ln, sizeof(ln))) return false;
    return Call(gm, f);
}

DWORD WINAPI Thread(LPVOID) {
    // 1. Wait for gameplay (the harness boots past OMEGA into untitled_1).
    std::wstring w;
    for (int i = 0; i < 90 && !coop::shutdown::IsShuttingDown(); ++i) {
        w = WorldNameGT();
        if (InGameplay(w)) break;
        ::Sleep(1000);
    }
    if (!InGameplay(w)) {
        UE_LOGW("menu_proceed: never reached gameplay (world='%ls') -- cannot transition to menu", w.c_str());
        g_running.store(false);
        return 0;
    }
    UE_LOGW("menu_proceed: in gameplay (world='%ls') -- transitioning to /Game/menu for the menu shot", w.c_str());

    // 2. Arm a transparent bypass for the dying-world teardown, then transition. The
    // bypass auto-expires after 14 s so our layer is active again at the loaded menu.
    auto ok = std::make_shared<int>(0);
    GT::Post([ok] {
        GT::SetTransparentBypass(14000);
        if (CallTransition(L"/Game/menu")) *ok = 1;
        UE_LOGW("menu_proceed: armed bypass(14s) + transition(/Game/menu) dispatched=%d", *ok);
    });

    // 3. Wait for the bypass to expire + the menu to finish loading (our layer active).
    ::Sleep(15000);

    // 4. Force the inject DETERMINISTICALLY -- don't rely on the Tick observer firing
    // in the brief window before our-layer-resume reverts the menu (~25 s+ after the
    // transition). ForceInjectNow injects onto the live ui_menu_C right now.
    GT::Post([] { coop::multiplayer_menu::ForceInjectNow(); });

    // 5. Let the inject land, then signal the capture (well before the menu reverts).
    ::Sleep(2500);
    UE_LOGW("menu_proceed: MENU-SHOT READY");  // capture the window here
    g_running.store(false);
    return 0;
}

}  // namespace

void Init() {
    char v[8] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_MENU_PROCEED", v, sizeof(v)) == 0 || v[0] != '1') return;
    UE_LOGW("menu_proceed: VOTVCOOP_MENU_PROCEED=1 -- TEST helper armed (gameplay -> transition to main menu)");
    if (g_running.exchange(true)) return;
    if (HANDLE t = ::CreateThread(nullptr, 0, &Thread, nullptr, 0, nullptr)) ::CloseHandle(t);
    else g_running.store(false);
}

}  // namespace coop::dev::menu_proceed
