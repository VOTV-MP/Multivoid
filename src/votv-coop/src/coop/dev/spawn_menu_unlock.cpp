// coop/dev/spawn_menu_unlock.cpp -- see header.

#include "coop/dev/spawn_menu_unlock.h"

#include "coop/dev/dev_gate.h"
#include "coop/ini_config.h"
#include "coop/host/shutdown.h"

#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/spawn_menu.h"

#include <windows.h>

#include <atomic>

namespace coop::dev::spawn_menu_unlock {

namespace GT = ue_wrap::game_thread;

namespace {

// The Q-key watcher is on iff this is true. Read by the watcher thread, written
// by SetEnabled (menu/boot).
std::atomic<bool> g_enabled{false};

bool KeyDown(int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; }

// Watches the Q key while enabled and opens the spawn menu on the rising edge.
// Started lazily on the first enable (no idle worker for players who never use
// this dev feature). Mirrors freecam's InputDriverThread:
//   - foreground-gated (GetAsyncKeyState is process-GLOBAL, so a same-box
//     host+client pair would otherwise double-fire from one Q press);
//   - the actual UFunction call is POSTED to the game thread (never touch the
//     engine from this worker thread);
//   - dev_gate re-checked per poll so the watcher self-disables the moment this
//     peer becomes a connected client.
DWORD WINAPI KeyWatcherThread(LPVOID) {
    bool prevRawQ = false;  // RAW Q edge (independent of focus) so the diagnostics fire once per press
    while (!coop::shutdown::IsShuttingDown()) {
        if (g_enabled.load(std::memory_order_acquire)) {
            // Live role gate: a watcher enabled while solo/hosting dies the moment
            // this peer is a connected client (dev tools are host-only).
            if (!coop::dev_gate::Allowed()) {
                g_enabled.store(false, std::memory_order_release);
                UE_LOGW("spawn_menu_unlock: auto-off -- dev features are disabled "
                        "while connected as a client");
                prevRawQ = false;
                ::Sleep(16);
                continue;
            }
            const bool rawQ = KeyDown('Q');
            if (rawQ && !prevRawQ) {  // one event per physical Q press
                const bool focused = ::coop::ini_config::IsOurWindowForeground();
                if (focused) {
                    UE_LOGI("spawn_menu_unlock: Q pressed (foreground) -- posting Toggle() to the game thread");
                    // Toggle, not Open: Q both opens AND dismisses the menu. Open() leaves the player
                    // in GameAndUI/cursor mode; with only an open path the player gets stranded and can
                    // no longer interact with the world (the 2026-06-16 "all peers can't interact" bug).
                    GT::Post([] { ue_wrap::spawn_menu::Toggle(); });
                } else {
                    // The most common "nothing happens" cause: our window isn't foreground (the
                    // ImGui menu / another window has focus), so the press is intentionally ignored.
                    UE_LOGW("spawn_menu_unlock: Q pressed but our window is NOT foreground -- "
                            "ignoring (click into the game, then press Q)");
                }
            }
            prevRawQ = rawQ;
        } else {
            prevRawQ = false;
        }
        ::Sleep(16);  // 60 Hz, matching freecam's driver
    }
    return 0;
}

void EnsureThread() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    if (HANDLE t = ::CreateThread(nullptr, 0, &KeyWatcherThread, nullptr, 0, nullptr)) {
        ::CloseHandle(t);  // detached
    }
    UE_LOGI("spawn_menu_unlock: Q-key watcher thread started (story-mode prop spawn menu)");
}

// Trigger-FILE watcher (autonomous diagnosis path only). mp.py's spawnmenutest creates the
// file; we call OpenNow() (the same path the F1 button uses) + delete it. Lets the spawn-menu
// open be exercised + its diagnostics (activeInterface state / dispatch result) read from the
// log WITHOUT a physical Q press (which needs foreground + a key). No-op unless the env is set.
DWORD WINAPI FileTriggerThread(LPVOID) {
    wchar_t triggerPath[512] = {};
    ::GetEnvironmentVariableW(L"VOTVCOOP_SPAWNMENU_TRIGGER", triggerPath, 512);
    if (triggerPath[0] == L'\0') return 0;  // nothing to watch
    int ticks = 0;
    while (!coop::shutdown::IsShuttingDown()) {
        if (++ticks >= 16) {  // ~250 ms
            ticks = 0;
            if (::GetFileAttributesW(triggerPath) != INVALID_FILE_ATTRIBUTES) {
                UE_LOGI("spawn_menu_unlock: trigger file seen -> OpenNow() (autonomous diagnosis)");
                OpenNow();
                ::DeleteFileW(triggerPath);  // one-shot per file creation
            }
        }
        ::Sleep(16);
    }
    return 0;
}

}  // namespace

void Init() {
    if (!::coop::ini_config::MasterEnabled()) {
        UE_LOGI("spawn_menu_unlock: disabled by master switch ([dev] enabled=0)");
        return;
    }
    // Autonomous-diagnosis file trigger (VOTVCOOP_SPAWNMENU_TRIGGER): start the watcher so a
    // test can fire OpenNow() without a physical Q press. Independent of the Q-watcher ini gate.
    {
        wchar_t probe[8] = {};
        if (::GetEnvironmentVariableW(L"VOTVCOOP_SPAWNMENU_TRIGGER", probe, 8) > 0) {
            if (HANDLE t = ::CreateThread(nullptr, 0, &FileTriggerThread, nullptr, 0, nullptr)) {
                ::CloseHandle(t);  // detached
            }
            UE_LOGI("spawn_menu_unlock: file-trigger ENABLED (VOTVCOOP_SPAWNMENU_TRIGGER) -- OpenNow on file");
        }
    }
    if (!::coop::ini_config::IsIniKeyTrue("spawn_menu_unlock")) {
        UE_LOGI("spawn_menu_unlock: off at boot (set [dev] spawn_menu_unlock=1 to "
                "force-enable; the F1 menu toggles it under [dev] devkeys)");
        return;
    }
    SetEnabled(true);
}

void SetEnabled(bool on) {
    if (on && !coop::dev_gate::Allowed()) {
        UE_LOGW("spawn_menu_unlock: REFUSED -- dev features are disabled while "
                "connected as a client");
        return;
    }
    if (on) EnsureThread();
    g_enabled.store(on, std::memory_order_release);
    UE_LOGI("spawn_menu_unlock: Q-key spawn menu %s", on ? "ENABLED (story mode)" : "disabled");
}

bool IsEnabled() { return g_enabled.load(std::memory_order_acquire); }

void OpenNow() {
    if (!coop::dev_gate::Allowed()) {
        UE_LOGW("spawn_menu_unlock: OpenNow REFUSED -- dev features are disabled "
                "while connected as a client");
        return;
    }
    GT::Post([] { ue_wrap::spawn_menu::Open(); });
}

}  // namespace coop::dev::spawn_menu_unlock
