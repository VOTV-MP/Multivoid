// coop/shutdown.cpp -- see shutdown.h.

#include "coop/session/shutdown.h"

#include "coop/net/session.h"
#include "coop/items/player_inventory_sync.h"  // v73: flush inventories on shutdown
#include "ue_wrap/game_thread.h"
#include "ue_wrap/hook.h"
#include "ue_wrap/log.h"

#include <windows.h>

#include <atomic>
#include <mutex>

namespace coop::shutdown {

// Global atomic shutdown flag -- file-private (internal linkage). Read via the public
// IsShuttingDown(); the SOLE writer is DoShutdown()'s compare_exchange. Once tripped it
// NEVER clears (process is going down). Was an extern in shutdown.h until the 2026-07-07
// boundary pass; no external writer ever existed, so it needs no public setter.
static std::atomic<bool> g_shuttingDown{false};

namespace {

// Atomic single-HWND subclass tracking. Held as atomics so the wndproc
// hot path can read them WITHOUT a mutex -- a mutex in CoopWndProc
// would contend with every Win32 message on the UI thread (thousands
// per second), and a per-tick Install() on the game thread holding the
// same mutex starves the UI thread -> black screen (bug observed
// 2026-05-26 v3). Atomic pair: HWND + its original wndproc pointer.
std::atomic<HWND>    g_subclassedHwnd{nullptr};
std::atomic<WNDPROC> g_origProc{nullptr};

// Slower-path state (Install / DoShutdown / UpdateWindowTitle). These
// run at most ~20 Hz from the timeline thread + once from WM_CLOSE, so
// mutex contention here is benign.
std::mutex g_slowMu;
bool g_didShutdown = false;
bool g_titled = false;
coop::net::Session* g_session = nullptr;

// EnumWindows callback -- pick the BEST candidate UnrealWindow-class
// top-level visible window owned by THIS process, by client area.
// UE4.27 destroys + recreates the main game HWND during world load
// (splash -> level transition applies game user settings). Per-tick
// Install() picks the current best; if it changed, we restore the old
// wndproc + subclass the new one. Most VOTV runs only have ONE
// UnrealWindow visible at a time so this is usually unambiguous.
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

LRESULT CALLBACK CoopWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Direct user-close signals -- no veto path, fire DoShutdown BEFORE
    // forwarding so our PE detour comes down before UE4 starts its
    // teardown PE calls:
    //   * WM_CLOSE -- canonical "please close" (rare; UE4.27 intercepts
    //     SC_CLOSE first).
    //   * WM_SYSCOMMAND wp&0xFFF0 == SC_CLOSE -- X-click / Alt+F4.
    //     UE4.27's FWindowsApplication::ProcessMessage calls its
    //     MessageHandler->OnWindowClose() directly on this, bypassing
    //     WM_CLOSE entirely. Hands-on 2026-05-26 v4 confirmed.
    // The low 4 bits of WM_SYSCOMMAND wParam are reserved; mask 0xFFF0.
    const bool isDirectClose =
        msg == WM_CLOSE ||
        (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_CLOSE);
    if (isDirectClose) {
        UE_LOGI("shutdown: close-signal received on HWND=%p msg=0x%X wp=0x%X",
                hwnd, msg, static_cast<unsigned>(wp));
        DoShutdown();
    }

    // Forward to UE4's wndproc.
    WNDPROC orig = g_origProc.load(std::memory_order_acquire);
    LRESULT result = orig ? ::CallWindowProcW(orig, hwnd, msg, wp, lp)
                          : ::DefWindowProcW(hwnd, msg, wp, lp);

    // WM_QUERYENDSESSION has a VETO path -- UE4 (or default) may
    // return FALSE to refuse OS shutdown. If we DoShutdown'd
    // unconditionally before forwarding (the audit-fix bug 2026-05-26 v5)
    // and UE4 vetoed, the OS keeps the process alive but our coop
    // subsystem is permanently dead with no recovery -- worse than
    // the original X-close hang. Only DoShutdown when UE4 (or default)
    // returns TRUE meaning OS will actually shut us down. Audit-fix.
    if (msg == WM_QUERYENDSESSION && result != 0) {
        UE_LOGI("shutdown: WM_QUERYENDSESSION approved by UE4 -- running cleanup");
        DoShutdown();
    }
    return result;
}

}  // namespace

void Install(coop::net::Session* session) {
    // Cheap fast-path: pick the current best HWND. If it matches what
    // we already subclassed, no work to do.
    HWND best = FindBestUnrealWindow();
    HWND curHwnd = g_subclassedHwnd.load(std::memory_order_acquire);
    if (best == curHwnd) {
        // Already on the right HWND (or both null = window not ready
        // yet, retry next tick). Still record session for later.
        if (session) {
            std::lock_guard<std::mutex> lk(g_slowMu);
            if (!g_session) g_session = session;
        }
        return;
    }
    if (!best) return;  // no UnrealWindow visible yet

    // Either no prior subclass or the HWND changed (engine recreated
    // the window during world load). Slow-path: lock, swap.
    std::lock_guard<std::mutex> lk(g_slowMu);
    if (!g_session && session) g_session = session;

    // Restore the prior HWND's wndproc if it still exists. Best-effort
    // (the engine may have already destroyed it during world-load).
    HWND prior = g_subclassedHwnd.load(std::memory_order_acquire);
    if (prior && ::IsWindow(prior)) {
        WNDPROC priorOrig = g_origProc.load(std::memory_order_acquire);
        if (priorOrig) {
            ::SetWindowLongPtrW(prior, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(priorOrig));
        }
        UE_LOGI("shutdown: HWND changed -- restored prior HWND=%p wndproc=%p",
                prior, priorOrig);
    }

    // Subclass the new HWND.
    WNDPROC orig = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtrW(best, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&CoopWndProc)));
    if (!orig) {
        UE_LOGW("shutdown: SetWindowLongPtrW failed on HWND=%p err=%lu", best, ::GetLastError());
        return;
    }
    // Publish atomically -- order matters: store origProc FIRST (so the
    // wndproc has a valid forward target if a message arrives between
    // these two stores; the OLD g_subclassedHwnd is still pointed at,
    // but our wndproc is the one being called for `best` now, so the
    // forward target it loads IS the new orig -- correct).
    g_origProc.store(orig, std::memory_order_release);
    g_subclassedHwnd.store(best, std::memory_order_release);

    // Re-title on new HWND.
    g_titled = false;

    // Diagnostic dump.
    wchar_t title[128] = {};
    ::GetWindowTextW(best, title, 128);
    RECT r{};
    ::GetWindowRect(best, &r);
    UE_LOGI("shutdown: subclassed HWND=%p title='%ls' rect=(%ld,%ld %ldx%ld) origProc=%p",
            best, title, r.left, r.top, r.right - r.left, r.bottom - r.top, orig);
}

void UpdateWindowTitle() {
    HWND h = g_subclassedHwnd.load(std::memory_order_acquire);
    if (!h) return;
    std::lock_guard<std::mutex> lk(g_slowMu);
    if (g_titled || !g_session) return;
    if (!g_session->running()) return;
    const bool isHost = (g_session->role() == coop::net::Role::Host);
    const wchar_t* title = isHost ? L"VotV (Host)" : L"VotV (Client)";
    ::SetWindowTextW(h, title);
    g_titled = true;
    UE_LOGI("shutdown: window title set to '%ls' on HWND=%p", title, h);
}

bool IsShuttingDown() {
    return g_shuttingDown.load(std::memory_order_acquire);
}

void DoShutdown() {
    bool expected = false;
    if (!g_shuttingDown.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel)) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_slowMu);
    if (g_didShutdown) return;
    g_didShutdown = true;

    UE_LOGI("shutdown: BEGIN cleanup (flag set; session=%p hwnd=%p)",
            g_session, g_subclassedHwnd.load(std::memory_order_relaxed));

    // v73: flush each connected peer's last inventory blob to <guid>.json BEFORE the session
    // stops (after Stop the slots are gone). Pure file I/O on captured bytes -- safe here.
    coop::player_inventory_sync::FlushAllToDisk();

    if (g_session) g_session->Stop();
    ::Sleep(100);  // let detached pollers observe g_shuttingDown
    ue_wrap::game_thread::Uninstall();
    ue_wrap::hook::Shutdown();

    UE_LOGI("shutdown: END cleanup");
}

}  // namespace coop::shutdown
