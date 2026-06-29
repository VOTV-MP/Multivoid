#include "coop/dev/pos_hud.h"

#include "coop/dev/dev_gate.h"
#include "coop/player/players_registry.h"
#include "coop/session/shutdown.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace coop::dev::pos_hud {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

namespace {

// Currently SHOWN on screen? Starts OFF -- the menu checkbox toggles it. Atomic
// because the menu (render thread) writes it via SetVisible and the refresh-pump
// thread reads it; the game thread only reads it inside posted tasks.
std::atomic<bool> g_active{false};

// Widget pointers. Created lazily on the first show (the GameInstance must
// exist by then; before that nothing in the world is meaningful to read anyway).
// Owned by the engine via the GameInstance outer -- not destroyed on level
// changes; we just RemoveFromViewport / AddToViewport to hide/show.
void* g_root = nullptr;
void* g_text = nullptr;

// Refresh-pump thread started once, lazily, on the first show.
std::atomic<bool> g_pumpStarted{false};

constexpr int kZOrder = 95;  // dev position HUD -- UMG viewport widget z-order

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
    // 2026-05-26: use the central coop::local_player registry instead of
    // raw FindObjectByClass -- the latter could return the PUPPET (a
    // mainPlayer_C orphan) and display its position as if it were the
    // local player. local_player::Get() filters via controller-not-null
    // and caches.
    void* local = coop::players::Registry::Get().Local();
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

// Refresh pump: while the overlay is visible, post a Refresh() to the game thread
// every ~100 ms (10 Hz is plenty for a numeric readout; faster just burns UFunction
// calls). Idles cheaply (one atomic load + sleep) while hidden. No key polling --
// the menu drives visibility via SetVisible.
DWORD WINAPI RefreshPumpThread(LPVOID) {
    int refreshCounter = 0;
    constexpr int kRefreshEveryN = 12;  // 12 * 16 ms = ~192 ms -> ~5 Hz
    while (!coop::shutdown::IsShuttingDown()) {
        // Live role gate: a readout shown BEFORE joining hides itself the
        // moment this peer is a connected client (coop::dev_gate).
        if (g_active.load() && !coop::dev_gate::Allowed()) {
            g_active.store(false, std::memory_order_release);
            GT::Post([] { Hide(); });
            UE_LOGW("pos_hud: auto-off -- dev features are disabled while connected as a client");
        }
        if (g_active.load() && ++refreshCounter >= kRefreshEveryN) {
            refreshCounter = 0;
            GT::Post([] { if (g_active.load()) Refresh(); });
        }
        ::Sleep(16);  // 60 Hz (user-set 2026-06-04, was 125)
    }
    return 0;
}

// Start the refresh pump once, on the first show (no idle thread for players who
// never open this dev tool).
void EnsurePump() {
    if (g_pumpStarted.exchange(true)) return;
    if (HANDLE t = ::CreateThread(nullptr, 0, &RefreshPumpThread, nullptr, 0, nullptr)) {
        ::CloseHandle(t);  // detached; loops until shutdown
    }
}

}  // namespace

void SetVisible(bool on) {
    if (on && !coop::dev_gate::Allowed()) {
        UE_LOGW("pos_hud: REFUSED -- dev features are disabled while connected as a client");
        return;
    }
    g_active.store(on, std::memory_order_release);
    if (on) {
        EnsurePump();
        GT::Post([] { Show(); });
    } else {
        GT::Post([] { Hide(); });
    }
    UE_LOGI("pos_hud: SetVisible(%d)", on ? 1 : 0);
}

bool IsVisible() { return g_active.load(std::memory_order_acquire); }

}  // namespace coop::dev::pos_hud
