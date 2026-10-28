// coop/dev/menu_proceed.cpp -- see coop/dev/menu_proceed.h. TEST-ONLY.
//
// Reaching VOTV's MAIN MENU autonomously is blocked at boot by the OMEGA content-warning screen:
// a special early-boot widget, not ui_menu_C, carrying no standard UButtons, and VOTV's menu
// cursor is raw-input driven, so synthetic OS clicks do not activate it either.
//
// So instead of fighting OMEGA this goes the other way: from whatever world the game boots into
// (the preload level, a fresh game, a loaded save), issue VOTV's own
// AmainGamemode_C::transition("/Game/menu"). Quitting to the menu loads the real ui_menu_C main
// menu without re-showing OMEGA, and our ui_menu_C::Tick observer (coop::multiplayer_menu)
// injects the MULTIPLAYER button. A transparent ProcessEvent bypass covers the dying-world
// teardown, where our detour otherwise hangs on the EndPlay storm (autotest_menutravel_probe is
// what shows that); it auto-expires, so the observers re-engage at the loaded menu.
//
// Needs no gameplay boot: any world with a live gamemode works, and the menu level itself skips this.

#include "coop/dev/menu_proceed.h"

#include "ui/multiplayer_menu.h"
#include "coop/session/shutdown.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

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

// What the thread reads off the game each poll. Every field is read on the game thread.
struct Probe {
    std::wstring world = L"<null>";   // the live UWorld leaf name
    bool gamemodeLive = false;        // a live mainGamemode_C: the object transition() is called on
    bool menuLive     = false;        // a live ui_menu_C: the main menu is ticking
    bool injected     = false;        // our MULTIPLAYER button is on that menu
};

// Run `body` on the game thread and wait for it. False when the game thread did not get to it
// within the wait (a long script stall, or the transparent bypass, which skips the task pump).
template <typename Body>
bool OnGameThread(Body body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body] { body(); done->store(1); });
    for (int i = 0; i < 1600 && done->load() == 0; ++i) ::Sleep(5);
    return done->load() != 0;
}

// One reading. With `inject` it first forces the MULTIPLAYER inject onto the live menu, so the
// confirmation below reads the result of that same task. False when the reading did not happen.
bool ReadProbe(Probe& out, bool inject) {
    auto p = std::make_shared<Probe>();
    const bool ok = OnGameThread([p, inject] {
        if (void* w = R::FindObjectByClass(prof::name::WorldClass)) p->world = R::ToString(R::NameOf(w));
        void* gm = R::FindObjectByClass(prof::name::GamemodeClass);
        p->gamemodeLive = gm && R::IsLive(gm);
        void* menu = R::FindObjectByClass(prof::name::UiMenuClass);
        p->menuLive = menu && R::IsLive(menu);
        if (inject && p->menuLive) coop::multiplayer_menu::ForceInjectNow();
        p->injected = p->menuLive && coop::multiplayer_menu::IsInjectedInto(menu);
    });
    if (ok) out = *p;
    return ok;
}

// The menu level is the world named "menu"; a world not yet created reads "<null>".
bool IsMenuLevel(const std::wstring& world) { return world == L"menu"; }
bool HasWorld(const std::wstring& world) { return world != L"<null>"; }

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

// Every wait below is a poll of the thing it waits for, once a second, bounded by the same number
// of tries; a bound that runs out ends the run with a FAIL line and never prints READY.
constexpr int kMaxTries = 90;

void Fail(const char* step, const Probe& p) {
    UE_LOGE("menu_proceed FAIL: %s (world='%ls' gamemode=%d menu=%d injected=%d)", step,
            p.world.c_str(), p.gamemodeLive ? 1 : 0, p.menuLive ? 1 : 0, p.injected ? 1 : 0);
    g_running.store(false);
}

DWORD WINAPI Thread(LPVOID) {
    Probe p;
    // 1+2. The transition is ready when a mainGamemode_C is live and the world is a real one other
    // than the menu level: whichever world the game booted into (the fresh game's, the preload level,
    // a loaded save) the game's own transition travels from it. A world that already IS the menu level
    // skips the transition. The call is retried each second until it dispatches. The bypass for the
    // dying-world teardown is armed in the same game-thread task as the call and cleared again if
    // the call did not dispatch, so a refused attempt leaves the task pump running.
    bool dispatched = false;
    bool ready = false;
    for (int i = 0; i < kMaxTries && !dispatched && !coop::shutdown::IsShuttingDown(); ++i) {
        if (i > 0) ::Sleep(1000);
        if (!ReadProbe(p, false)) continue;
        if (IsMenuLevel(p.world)) {
            UE_LOGW("menu_proceed: the world is already the menu level -- no transition needed");
            dispatched = true;
            break;
        }
        if (!HasWorld(p.world) || !p.gamemodeLive) continue;
        if (!ready) {
            ready = true;
            UE_LOGW("menu_proceed: world='%ls' with a live gamemode -- transitioning to /Game/menu for the menu shot",
                    p.world.c_str());
        }
        auto ok = std::make_shared<std::atomic<int>>(0);
        if (!OnGameThread([ok] {
                GT::SetTransparentBypass(14000);
                if (CallTransition(L"/Game/menu")) ok->store(1);
                else GT::SetTransparentBypass(0);
                UE_LOGW("menu_proceed: armed bypass(14s) + transition(/Game/menu) dispatched=%d",
                        ok->load());
            }))
            continue;
        dispatched = ok->load() != 0;
    }
    if (!dispatched) {
        Fail(ready ? "the transition never dispatched" : "no world with a live gamemode to travel from", p);
        return 0;
    }

    // 3. The menu is up when the world is the menu level, a ui_menu_C is live and the bypass has expired
    // (a task posted under the bypass does not run, so it is checked first).
    bool menuUp = false;
    for (int i = 0; i < kMaxTries && !menuUp && !coop::shutdown::IsShuttingDown(); ++i) {
        ::Sleep(1000);
        if (GT::TransparentBypassActive()) continue;
        menuUp = ReadProbe(p, false) && IsMenuLevel(p.world) && p.menuLive;
    }
    if (!menuUp) {
        Fail("the main menu never came up after the transition", p);
        return 0;
    }

    // 4. Force the inject DETERMINISTICALLY -- don't rely on the Tick observer firing in the brief
    // window before our-layer-resume reverts the menu (~25 s+ after the transition) -- and signal the
    // capture once the inject is confirmed on that menu.
    bool injected = false;
    for (int i = 0; i < kMaxTries && !injected && !coop::shutdown::IsShuttingDown(); ++i) {
        if (i > 0) ::Sleep(500);
        injected = ReadProbe(p, true) && p.injected;
    }
    if (!injected) {
        Fail("the MULTIPLAYER button was not injected", p);
        return 0;
    }
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
