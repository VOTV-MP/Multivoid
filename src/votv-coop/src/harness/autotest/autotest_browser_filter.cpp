// harness/autotest/autotest_browser_filter.cpp -- TEST-ONLY: the server browser's "Hide incompatible servers" checkbox, at the
// main menu against a stand-in master that lists three lobbies we can join and two we cannot.
//
// The box is clicked with the real pointer. VOTVCOOP_BROWSER_FILTER_EXPECT=1 is the second launch: the box comes up as the first
// left it, and is then cleared. Gated by VOTVCOOP_RUN_BROWSER_FILTER.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "harness/screenshot.h"
#include "ui/input_focus.h"
#include "ui/server_browser_filter.h"
#include "ui/server_browser_native.h"
#include "ui/server_browser_rows.h"
#include "ui/server_browser_surface.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <atomic>
#include <string>

namespace harness::autotest {
namespace {

namespace F = ui::server_browser_filter;
namespace Rows = ui::server_browser_rows;
namespace U = ue_wrap::umg;

constexpr int kAllRows = 5, kCompatibleRows = 3;

int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Check(bool ok, const char* what) {
    UE_LOGI("browser-filter: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

bool WaitShown(int shown, int hidden) {
    int s = -1, h = -1;
    for (int i = 0; i < 100; ++i) {
        OnGameThread([&] { s = Rows::ShownRows(); h = Rows::HiddenRows(); });
        if (s == shown && h == hidden) break;
        ::Sleep(100);
    }
    UE_LOGI("browser-filter: shown %d (want %d), hidden %d (want %d)", s, shown, h, hidden);
    return s == shown && h == hidden;
}

// The real pointer: the same release edge a player makes.
bool ClickBox() {
    ue_wrap::FVector2D tl{}, sz{};
    bool ok = false;
    OnGameThread([&] {
        void* box = F::Box();
        ok = box && U::WidgetScreenRect(box, tl, sz);
    });
    if (!ok) return false;
    bool foreground = false;
    for (int i = 0; i < 50 && !foreground; ++i) {
        OnGameThread([&] { foreground = ui::input_focus::IsOurWindowForeground(); });
        if (!foreground) ::Sleep(100);
    }
    if (!foreground) {
        UE_LOGW("browser-filter: VERDICT ABORT -- the game window is not in the foreground, so a click would land elsewhere");
        return false;
    }
    ::SetCursorPos(static_cast<int>(tl.X + 13.f), static_cast<int>(tl.Y + sz.Y / 2.f));
    ::Sleep(300);
    ::mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    ::Sleep(120);
    ::mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    ::Sleep(400);
    return true;
}

}  // namespace

DWORD WINAPI BrowserFilterThread(LPVOID /*arg*/) {
    const bool secondLaunch = coop::config::ReadEnv("VOTVCOOP_BROWSER_FILTER_EXPECT") == "1";
    ::Sleep(15000);   // the menu up
    bool open = false;
    for (int i = 0; i < 1200 && !open; ++i) {
        // An open waits for a menu tick and expires: the content warning stands in front of the menu for a while.
        if (i % 150 == 0) OnGameThread([] { ui::server_browser_surface::Open(); });
        ::Sleep(100);
        OnGameThread([&] { open = ui::server_browser_native::IsOpen(); });
    }
    Check(open, "the browser opened");
    if (!open) return 0;

    if (secondLaunch) {
        bool on = false;
        OnGameThread([&] { on = F::HideIncompatible(); });
        Check(on, "the checkbox came up as the last launch left it");
        Check(WaitShown(kCompatibleRows, kAllRows - kCompatibleRows), "and the list is filtered from the start");
        harness::screenshot::Capture(L"browser-filter-relaunch");
        if (!ClickBox()) return 0;
        Check(WaitShown(kAllRows, 0), "unchecking brings the incompatible servers back");
        UE_LOGI("browser-filter: DONE fails=%d", g_fails);
        return 0;
    }

    bool on = true;
    OnGameThread([&] { on = F::HideIncompatible(); });
    Check(!on, "the checkbox starts unchecked");
    Check(WaitShown(kAllRows, 0), "unchecked, every server shows");
    ::Sleep(500);
    harness::screenshot::Capture(L"browser-filter-unchecked");

    if (!ClickBox()) return 0;
    OnGameThread([&] { on = F::HideIncompatible(); });
    Check(on, "a click checks it");
    Check(WaitShown(kCompatibleRows, kAllRows - kCompatibleRows), "checked, the incompatible servers are gone");
    harness::screenshot::Capture(L"browser-filter-checked");
    // A fetch must not bring them back: the master's list is fetched every 5 s.
    ::Sleep(6500);
    Check(WaitShown(kCompatibleRows, kAllRows - kCompatibleRows), "and a later fetch keeps them gone");

    if (!ClickBox()) return 0;
    Check(WaitShown(kAllRows, 0), "unchecking shows them again");
    if (!ClickBox()) return 0;
    Check(WaitShown(kCompatibleRows, kAllRows - kCompatibleRows), "checking again hides them, and this is what the next launch reads");
    UE_LOGI("browser-filter: DONE fails=%d", g_fails);
    return 0;
}

}  // namespace harness::autotest
