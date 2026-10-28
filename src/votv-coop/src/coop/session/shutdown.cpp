// coop/session/shutdown.cpp -- see shutdown.h.

#include "ue_wrap/core/gc_pin.h"
#include "coop/session/shutdown.h"

#include "coop/input/game_window_proc.h"
#include "coop/net/session.h"
#include "ui/overlay_backend.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/uobject_listeners.h"

#include <windows.h>

#include <atomic>
#include <mutex>

namespace coop::shutdown {

// Global atomic shutdown flag -- file-private (internal linkage). Read through the public
// IsShuttingDown(); the only writers are in this file, DoShutdown()'s compare_exchange and
// the process-exit persist. Once tripped it NEVER clears, the process being on its way
// down, and it needs no public setter because no external writer exists.
static std::atomic<bool> g_shuttingDown{false};

namespace {

// Slower-path state (SetSession / DoShutdown / UpdateWindowTitle). These run at most once per
// frame tail on the game thread + once from the close message, so mutex contention here is benign.
// The window message path (OnWindowMessage) takes no lock: it runs for every message of the UI
// thread, thousands per second.
std::mutex g_slowMu;
bool g_didShutdown = false;
HWND g_titledHwnd = nullptr;   // the window whose title is already set
coop::net::Session* g_session = nullptr;

}  // namespace

void SetSession(coop::net::Session* session) {
    if (!session) return;
    std::lock_guard<std::mutex> lk(g_slowMu);
    if (!g_session) g_session = session;
}

void OnWindowMessage(void* hwnd, unsigned msg, uintptr_t wp) {
    // Direct user-close signals -- no veto path, so fire DoShutdown BEFORE
    // forwarding and bring our ProcessEvent detour down before UE4 starts its own
    // teardown dispatches:
    //   * WM_CLOSE -- the canonical "please close", rare, because UE4.27
    //     intercepts SC_CLOSE first.
    //   * WM_SYSCOMMAND with wp & 0xFFF0 == SC_CLOSE -- the X click and Alt+F4.
    //     UE4.27's FWindowsApplication::ProcessMessage calls its
    //     MessageHandler->OnWindowClose() directly on this and never sends
    //     WM_CLOSE at all.
    // The low 4 bits of WM_SYSCOMMAND wParam are reserved; mask 0xFFF0.
    const bool isDirectClose =
        msg == WM_CLOSE ||
        (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_CLOSE);
    if (isDirectClose) {
        UE_LOGI("shutdown: close-signal received on HWND=%p msg=0x%X wp=0x%X",
                hwnd, msg, static_cast<unsigned>(wp));
        DoShutdown();
    }
}

void OnWindowMessageForwarded(unsigned msg, intptr_t result) {
    // WM_QUERYENDSESSION has a VETO path: UE4, or the default handler, may return FALSE and
    // refuse the OS shutdown. Shutting down unconditionally before forwarding would leave the
    // coop subsystem permanently dead with no recovery in a process the OS then keeps alive --
    // worse than the close hang this whole path exists to fix. So DoShutdown only when the
    // forward returns TRUE, meaning the OS really will take us down.
    if (msg == WM_QUERYENDSESSION && result != 0) {
        UE_LOGI("shutdown: WM_QUERYENDSESSION approved by UE4 -- running cleanup");
        DoShutdown();
    }
}

void UpdateWindowTitle() {
    HWND h = static_cast<HWND>(coop::input::game_window_proc::Hwnd());
    if (!h) return;
    std::lock_guard<std::mutex> lk(g_slowMu);
    if (g_titledHwnd == h || !g_session) return;
    if (!g_session->running()) return;
    const bool isHost = (g_session->role() == coop::net::Role::Host);
    const wchar_t* title = isHost ? L"VotV (Host)" : L"VotV (Client)";
    ::SetWindowTextW(h, title);
    g_titledHwnd = h;
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
            g_session, coop::input::game_window_proc::Hwnd());

    if (g_session) g_session->Stop();
    ::Sleep(100);  // let detached pollers observe g_shuttingDown
    ue_wrap::game_thread::Uninstall();
    ue_wrap::uobject_listeners::Uninstall();
    ui::overlay_backend::RestoreEngineSlots();  // the Vulkan overlay's two loader slots, with the hooks lifted
    ue_wrap::hook::Shutdown();
    // From here on a GC pin releases WITHOUT touching the engine. Pins can live inside
    // statics (the proxy map, the permanent audio pins), so their destructors run at DLL
    // unload, long after this point -- and un-rooting then reads an object's InternalIndex
    // and walks GUObjectArray for a collector that will never run again.
    ue_wrap::GcPin::StopReleases();

    UE_LOGI("shutdown: END cleanup");
}

void FlushLogAtProcessExit() {
    // See the header for the census of what this deliberately does NOT do and why. Everything
    // here must be safe under the loader lock with every other thread already terminated: no
    // mutex, no join, no socket, no hook teardown, no sleep.
    //
    // The flag first, and via the same atomic DoShutdown uses -- it costs nothing and it means
    // any thread that somehow survived stops on its next check.
    g_shuttingDown.store(true, std::memory_order_release);

    // One line, then flush the log: on this path there is no later opportunity, and the line is
    // the only evidence the branch ran at all.
    UE_LOGI("shutdown: process exit (teardown deliberately skipped -- loader lock)");
    ue_wrap::log::Flush();
}

}  // namespace coop::shutdown
