// coop/input/game_window_proc.cpp -- see coop/input/game_window_proc.h.

#include "coop/input/game_window_proc.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/session/shutdown.h"
#include "ue_wrap/core/log.h"
#include "ui/imgui_overlay.h"

#include <windows.h>

#include <atomic>
#include <cwchar>

namespace coop::input::game_window_proc {
namespace {

// The window and the procedure it had before ours. Atomics, so the window procedure reads them
// without a lock: a mutex there would contend with every message of the UI thread. Tick is the
// only writer, on the game thread.
std::atomic<HWND>    g_hwnd{nullptr};
std::atomic<WNDPROC> g_origProc{nullptr};

// Tick's pacing; one writer at a time (harness::Start before the timeline thread exists, then the
// pump on the game thread).
ULONGLONG g_nextFindMs = 0;
constexpr ULONGLONG kFindEveryMs = 250;      // while no window is held
constexpr ULONGLONG kRecheckEveryMs = 1000;  // while one is: is there a larger visible window?

// The best candidate among the top-level visible windows of class UnrealWindow that belong to
// THIS process, by client area. UE4.27 destroys and recreates the main game window during world
// load (the splash-to-level transition applies the user settings); most runs have only one such
// window at a time, so the pick is unambiguous.
struct EnumCtx { DWORD pid; HWND best; long long bestArea; };
BOOL CALLBACK EnumCb(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<EnumCtx*>(lp);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid) return TRUE;
    if (!::IsWindowVisible(h)) return TRUE;
    if (::GetWindow(h, GW_OWNER) != nullptr) return TRUE;  // popup/owned
    wchar_t cls[64] = {};
    if (::GetClassNameW(h, cls, 64) <= 0) return TRUE;
    if (::wcscmp(cls, L"UnrealWindow") != 0) return TRUE;
    RECT r{};
    if (!::GetClientRect(h, &r)) return TRUE;
    const long long area = static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
    if (area > c->bestArea) { c->bestArea = area; c->best = h; }
    return TRUE;
}
HWND FindBestUnrealWindow() {
    EnumCtx ctx{::GetCurrentProcessId(), nullptr, 0};
    ::EnumWindows(&EnumCb, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

// The overlay is consulted first and the close handling second: with the close handler installed
// at boot and the overlay's hook on the first present, the overlay sat on top of the close
// handler in a running game. The X click reaches the close handler as WM_SYSCOMMAND, which the
// overlay never takes; Alt+F4 arrives as WM_SYSKEYDOWN, which a capturing surface swallows, so it
// closes nothing while one is up, as before.
LRESULT CALLBACK GameWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT swallowed = 0;
    if (ui::imgui_overlay::HandleWindowMessage(hwnd, msg, wp, lp, swallowed)) return swallowed;

    coop::shutdown::OnWindowMessage(hwnd, msg, wp);

    // Subclass publishes the original before ours goes in, so a null here is a broken invariant,
    // not a state to carry on from: say so once, and give the message to the default handler.
    WNDPROC orig = g_origProc.load(std::memory_order_acquire);
    LRESULT result;
    if (orig) {
        result = ::CallWindowProcW(orig, hwnd, msg, wp, lp);
    } else {
        static std::atomic<bool> sLogged{false};
        if (!sLogged.exchange(true))
            UE_LOGE("game_window_proc: a message reached the subclass with no original procedure "
                    "(HWND=%p msg=0x%X) -- forwarding to DefWindowProc", hwnd, msg);
        result = ::DefWindowProcW(hwnd, msg, wp, lp);
    }

    coop::shutdown::OnWindowMessageForwarded(msg, result);
    return result;
}

void Subclass(HWND best) {
    // The window's current procedure is published before ours replaces it, so no message can reach
    // GameWindowProc with nothing to forward to. SetWindowLongPtrW returns what was really there.
    WNDPROC before = reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(best, GWLP_WNDPROC));
    g_origProc.store(before, std::memory_order_release);
    WNDPROC orig = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtrW(best, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&GameWindowProc)));
    if (!orig) {
        // Retried by Tick at its own pace; the failure is logged once per window.
        static HWND sFailed = nullptr;
        if (sFailed != best) {
            sFailed = best;
            UE_LOGW("game_window_proc: SetWindowLongPtrW failed on HWND=%p err=%lu", best,
                    ::GetLastError());
        }
        return;
    }
    if (orig != before) g_origProc.store(orig, std::memory_order_release);
    g_hwnd.store(best, std::memory_order_release);

    wchar_t title[128] = {};
    ::GetWindowTextW(best, title, 128);
    RECT r{};
    ::GetWindowRect(best, &r);
    UE_LOGI("game_window_proc: subclassed HWND=%p title='%ls' rect=(%ld,%ld %ldx%ld) origProc=%p",
            best, title, r.left, r.top, r.right - r.left, r.bottom - r.top, orig);
}

long long ClientArea(HWND h) {
    RECT r{};
    if (!::GetClientRect(h, &r)) return 0;
    return static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
}

// Hand a still-living window its original procedure back. False when something subclassed it
// after us (our procedure is no longer the outermost), which cannot be undone from here; the
// caller then stays on this window.
bool Release(HWND h) {
    if (::GetWindowLongPtrW(h, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&GameWindowProc)) {
        static HWND sRefused = nullptr;
        if (sRefused != h) {
            sRefused = h;
            UE_LOGW("game_window_proc: HWND=%p was subclassed again after ours -- staying on it", h);
        }
        return false;
    }
    ::SetWindowLongPtrW(h, GWLP_WNDPROC,
                        reinterpret_cast<LONG_PTR>(g_origProc.load(std::memory_order_acquire)));
    g_hwnd.store(nullptr, std::memory_order_release);
    UE_LOGI("game_window_proc: released HWND=%p for a larger window", h);
    return true;
}

}  // namespace

void Tick() {
    // The red arm of the typing proof: never subclass, so no key can reach a native text field. Read
    // once; Tick is not per message.
    static const bool red = coop::config::ResolveFlag(coop::config_registry::rows::window_proc_red);
    if (red) {
        static bool sTold = false;
        if (!sTold) {
            sTold = true;
            UE_LOGW("game_window_proc: RED -- the window is not subclassed, so no key reaches a native "
                    "text field");
        }
        return;
    }
    HWND held = g_hwnd.load(std::memory_order_acquire);
    if (held && !::IsWindow(held)) {
        // A destroyed window takes its subclass with it, so there is nothing to restore.
        UE_LOGI("game_window_proc: HWND=%p was destroyed -- looking for its successor", held);
        g_hwnd.store(nullptr, std::memory_order_release);
        g_nextFindMs = 0;
        held = nullptr;
    }
    const ULONGLONG now = ::GetTickCount64();
    if (now < g_nextFindMs) return;
    g_nextFindMs = now + (held ? kRecheckEveryMs : kFindEveryMs);
    HWND best = FindBestUnrealWindow();
    if (!best || best == held) return;
    // A window is held: move only to a larger visible one (the engine can create the real window
    // while a smaller one lives), and only if the held window can be given its own procedure back,
    // since this subclass forwards to ONE original.
    if (held) {
        if (ClientArea(best) <= (::IsWindowVisible(held) ? ClientArea(held) : 0)) return;
        if (!Release(held)) return;
    }
    Subclass(best);
}

void* Hwnd() {
    return g_hwnd.load(std::memory_order_acquire);
}

}  // namespace coop::input::game_window_proc
