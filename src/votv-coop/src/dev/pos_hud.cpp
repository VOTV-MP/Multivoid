#include "dev/pos_hud.h"

#include "dev/common.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace dev::pos_hud {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

namespace {

// Read-once at Init; the hotkey thread checks this before doing anything.
bool g_iniEnabled = false;

// Currently SHOWN on screen? Starts OFF -- F2 toggles it. Atomic because the
// hotkey thread reads + writes it (the toggle is debounced there); the game
// thread only reads it inside posted tasks.
std::atomic<bool> g_active{false};

// Widget pointers. Created lazily on the first toggle-on (the GameInstance must
// exist by then; before that nothing in the world is meaningful to read anyway).
// Owned by the engine via the GameInstance outer -- not destroyed on level
// changes; we just RemoveFromViewport / AddToViewport to hide/show.
void* g_root = nullptr;
void* g_text = nullptr;

// Cached local pawn for Refresh -- never re-walk GUObjectArray per refresh tick
// (FindObjectByClass is O(NumObjects)~100k). Same pattern as harness::g_netLocal
// and engine.cpp::CamMgr: resolve once, IsLive-revalidate, re-find only on miss.
void* g_local = nullptr;

constexpr int kZOrder = 95;  // just below the coop hud_feed (which is at 100)

bool KeyDown(int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; }

// Build the LEFT-MIDDLE overlay widget. Outer must be a persistent UObject (the
// GameInstance) so it survives level loads; pivot at the widget's left-middle
// (alignment {0, 0.5}) placed at (40, 540) in viewport pixels -- tuned for the
// 1920x1080 test window. Game thread only.
bool LazyCreate() {
    // Stale-pointer guard: a level load may GC the widget even with a GameInstance
    // outer. If Show() is pressed BEFORE any Refresh runs (which has its own
    // IsLive self-heal), AddWidgetToViewport on a freed UObject would crash --
    // detect-and-null here so we re-create cleanly.
    if (g_root && !R::IsLive(g_root)) {
        UE_LOGI("pos_hud: widget GC'd (detected in LazyCreate); re-creating");
        g_root = g_text = nullptr;
    }
    if (g_root) return true;
    void* gi = R::FindObjectByClass(P::name::GameInstanceClass);
    if (!gi) {
        UE_LOGW("pos_hud: no GameInstance yet (not in gameplay) -- defer create");
        return false;
    }
    // Soft yellow so it reads against most VOTV backgrounds without screaming;
    // monospace look comes from the engine default (UMG TextBlock with a fixed
    // font; we don't have a monospace asset to point to, so digits drift a touch
    // in width -- live with it, this is a dev tool).
    const ue_wrap::FLinearColor kColor{1.0f, 0.95f, 0.3f, 1.0f};
    if (!E::SpawnScreenTextWidget(gi, kZOrder,
                                  ue_wrap::FVector2D{0.f, 0.5f},
                                  ue_wrap::FVector2D{40.f, 540.f},
                                  /*justify Left*/ 0, /*fontSize*/ 14,
                                  kColor, &g_root, &g_text)
        || !g_root || !g_text) {
        UE_LOGE("pos_hud: SpawnScreenTextWidget failed");
        g_root = g_text = nullptr;
        return false;
    }
    UE_LOGI("pos_hud: widget created (root=%p text=%p)", g_root, g_text);
    return true;
}

// Re-read the player + camera state and push the formatted text into the widget.
// Game thread only. If the local player isn't resolvable (boot / level change /
// OMEGA->story transition) the overlay shows a placeholder, never crashes.
void Refresh() {
    if (!g_text) return;
    // Self-heal: if the widget object was GC'd (level change), drop pointers so
    // the next toggle-on re-creates it.
    if (!R::IsLive(g_root)) {
        UE_LOGI("pos_hud: widget GC'd; will re-create on next toggle");
        g_root = g_text = nullptr;
        return;
    }

    wchar_t buf[256] = {};
    // Cache pattern (post-ship audit H1): one FindObjectByClass at boot / after a
    // level kills the pointer. NEVER walk GUObjectArray every refresh tick.
    if (g_local && !R::IsLive(g_local)) g_local = nullptr;
    if (!g_local) g_local = R::FindObjectByClass(P::name::MainPlayerClass);
    void* local = g_local;
    if (local) {
        const ue_wrap::FVector loc = E::GetActorLocation(local);
        const ue_wrap::FRotator cam = E::GetCameraRotation();
        std::swprintf(buf, sizeof(buf) / sizeof(buf[0]),
                      L"Pos   X %+8.0f   Y %+8.0f   Z %+8.0f\n"
                      L"Cam   yaw %+7.1f   pitch %+6.1f   roll %+5.1f",
                      loc.X, loc.Y, loc.Z, cam.Yaw, cam.Pitch, cam.Roll);
    } else {
        std::swprintf(buf, sizeof(buf) / sizeof(buf[0]),
                      L"Pos   (no player)\nCam   (no player)");
    }
    E::SetWidgetText(g_text, buf);
}

void Show() {
    if (!LazyCreate()) return;
    if (!E::AddWidgetToViewport(g_root, kZOrder)) {
        UE_LOGW("pos_hud: AddToViewport failed");
        return;
    }
    Refresh();  // paint immediately so there's no blank frame
    UE_LOGI("pos_hud: ON");
}

void Hide() {
    if (g_root && R::IsLive(g_root)) E::RemoveWidgetFromViewport(g_root);
    UE_LOGI("pos_hud: OFF");
}

// Hotkey + update pump. Polls F2 (rising edge) to toggle visibility, and while
// visible posts a Refresh() to the game thread every ~100 ms (10 Hz is plenty
// for a numeric readout; faster updates just burn UFunction calls). Sleeps
// short between iterations so the toggle response is snappy. Mirrors the
// freecam::HotkeyThread shape.
DWORD WINAPI HotkeyThread(LPVOID) {
    bool prevF2 = false;
    int refreshCounter = 0;
    constexpr int kRefreshEveryN = 12;  // 12 * 8 ms = ~96 ms -> ~10 Hz
    for (;;) {
        // Foreground-window gate: GetAsyncKeyState is GLOBAL across processes,
        // so pressing F2 in the client's window would otherwise fire the hotkey
        // in BOTH host + client (each runs its own pos_hud thread reading the
        // same global key state). Only react when OUR window is focused.
        const bool f2 = ::dev::IsOurWindowForeground() && KeyDown(VK_F2);
        if (f2 && !prevF2) {
            // Rising edge -> flip. Only this thread writes g_active, so plain
            // load+store is race-free (the game-thread Refresh callback reads it
            // through atomic load on its side).
            const bool newState = !g_active.load();
            g_active.store(newState);
            if (newState) GT::Post([] { Show(); });
            else          GT::Post([] { Hide(); });
        }
        prevF2 = f2;

        if (g_active.load() && ++refreshCounter >= kRefreshEveryN) {
            refreshCounter = 0;
            GT::Post([] { if (g_active.load()) Refresh(); });
        }

        ::Sleep(8);
    }
}

}  // namespace

void Init() {
    // Master kill-switch first: [dev] enabled=0 forces every dev feature off
    // regardless of the granular `posinfo=...` line.
    if (!::dev::MasterEnabled()) {
        UE_LOGI("pos_hud: disabled by master switch ([dev] enabled=0)");
        return;
    }
    g_iniEnabled = ::dev::IsIniKeyTrue("posinfo");
    if (!g_iniEnabled) {
        UE_LOGI("pos_hud: disabled (set [dev] posinfo=1 in votv-coop.ini to enable; F2 toggles)");
        return;
    }
    if (HANDLE t = ::CreateThread(nullptr, 0, &HotkeyThread, nullptr, 0, nullptr)) {
        ::CloseHandle(t);  // detached
    }
    UE_LOGI("pos_hud: ENABLED (F2 toggles; left-middle of screen)");
}

}  // namespace dev::pos_hud
