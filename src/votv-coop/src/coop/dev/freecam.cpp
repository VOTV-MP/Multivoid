#include "coop/dev/freecam.h"

#include "coop/dev/dev_gate.h"
#include "coop/player/players_registry.h"
#include "coop/session/shutdown.h"
#include "coop/session/ini_config.h"

#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

namespace coop::dev::freecam {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

namespace {

std::atomic<bool> g_active{false};      // freecam currently on
std::atomic<bool> g_movePosted{false};  // a MovementTick is already queued (no backlog)
std::atomic<unsigned long long> g_lastMs{0};
std::atomic<int> g_wheelNotches{0};     // mouse-wheel notches accrued (consumed by MovementTick)

constexpr float kBaseSpeed = 600.f;     // cm/s
float g_speed = kBaseSpeed;             // wheel-adjustable; game-thread only

// All touched only on the game thread (Enable/Disable/Teleport are posted; the
// movement tick runs inside the ProcessEvent detour).
void* g_camActor = nullptr;
void* g_pc = nullptr;
void* g_player = nullptr;
ue_wrap::FVector g_camPos;

constexpr float kFastMul = 4.f;
constexpr float kDeg2Rad = 0.01745329f;
constexpr float kMinSpeed = 60.f, kMaxSpeed = 12000.f;

bool KeyDown(int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; }

inline void* ReadPtr(void* base, size_t off) {
    return base ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off) : nullptr;
}

// ---- player-control freeze (user 2026-07-05): while freecam is on, WASD/Space/Ctrl fly
// the CAMERA -- the same keys must not also walk/jump the PAWN blindly. The engine seam is
// CharacterMovement's mode: DisableMovement (MOVE_None) stops locomotion AND jump physics
// wholesale, while LOOK (control rotation) stays live -- the freecam aims with it. Modes
// are setter-managed (transition side effects), so this goes through the reflected
// DisableMovement/SetMovementMode UFunctions, never a raw field write. The prior mode
// (+custom sub-mode) is captured at freeze and restored verbatim at unfreeze -- a zero-g /
// swimming player must come back to THAT state, not a hardcoded Walking. ----------------
void* g_cmcClass = nullptr;
void* g_disableMovementFn = nullptr;
void* g_setMovementModeFn = nullptr;
int32_t g_offMovementMode = -1;    // CMC.MovementMode (EMovementMode byte)
int32_t g_offCustomMode = -1;      // CMC.CustomMovementMode (byte)
uint8_t g_frozenPrevMode = 0;      // captured at freeze
uint8_t g_frozenPrevCustom = 0;
bool g_controlFrozen = false;      // a freeze is outstanding (restore owed on Disable)

bool ResolveFreezeFns() {
    if (!g_cmcClass) g_cmcClass = R::FindClass(L"CharacterMovementComponent");
    if (!g_cmcClass) return false;
    if (!g_disableMovementFn) g_disableMovementFn = R::FindFunction(g_cmcClass, L"DisableMovement");
    if (!g_setMovementModeFn) g_setMovementModeFn = R::FindFunction(g_cmcClass, L"SetMovementMode");
    if (g_offMovementMode < 0) g_offMovementMode = R::FindPropertyOffset(g_cmcClass, L"MovementMode");
    if (g_offCustomMode < 0) g_offCustomMode = R::FindPropertyOffset(g_cmcClass, L"CustomMovementMode");
    return g_disableMovementFn && g_setMovementModeFn && g_offMovementMode >= 0 &&
           g_offCustomMode >= 0;
}

void* PlayerCmc() {
    return (g_player && R::IsLive(g_player))
               ? ReadPtr(g_player, P::off::ACharacter_CharacterMovement)
               : nullptr;
}

void FreezePlayerControl() {
    if (!ResolveFreezeFns()) {
        UE_LOGW("freecam: player-control freeze unavailable (CMC fns/offsets unresolved) -- "
                "the pawn will still react to the fly keys");
        return;
    }
    void* cmc = PlayerCmc();
    if (!cmc) { UE_LOGW("freecam: no CharacterMovement on the local player -- freeze skipped"); return; }
    g_frozenPrevMode = *(reinterpret_cast<uint8_t*>(cmc) + g_offMovementMode);
    g_frozenPrevCustom = *(reinterpret_cast<uint8_t*>(cmc) + g_offCustomMode);
    ue_wrap::ParamFrame f(g_disableMovementFn);
    if (!f.valid()) return;
    ue_wrap::Call(cmc, f);
    g_controlFrozen = true;
    UE_LOGI("freecam: player control FROZEN (MovementMode %u->None, custom=%u captured)",
            g_frozenPrevMode, g_frozenPrevCustom);
}

void UnfreezePlayerControl() {
    if (!g_controlFrozen) return;
    g_controlFrozen = false;
    void* cmc = PlayerCmc();
    if (!cmc) {
        // Level reload freed the pawn -- the fresh pawn's CMC boots unfrozen; nothing owed.
        UE_LOGI("freecam: unfreeze skipped (player/CMC gone; a fresh pawn starts unfrozen)");
        return;
    }
    ue_wrap::ParamFrame f(g_setMovementModeFn);
    if (!f.valid()) return;
    f.Set<uint8_t>(L"NewMovementMode", g_frozenPrevMode);
    f.Set<uint8_t>(L"NewCustomMode", g_frozenPrevCustom);
    ue_wrap::Call(cmc, f);
    UE_LOGI("freecam: player control RESTORED (MovementMode -> %u, custom=%u)",
            g_frozenPrevMode, g_frozenPrevCustom);
}

// Walk GUObjectArray for THE local mainPlayer_C -- the one possessed by a
// Local-vs-puppet discriminator + caching now lives in coop::local_player
// per RULE 1 (2026-05-26 unification). This module just calls Get().

void Enable() {
    if (!::coop::dev_gate::Allowed()) {
        UE_LOGW("freecam: REFUSED -- dev features are disabled while connected as a client");
        return;
    }
    g_player = coop::players::Registry::Get().Local();
    if (!g_player) { UE_LOGW("freecam: no local player (no mainPlayer_C with a Controller)"); return; }
    g_pc = E::GetController(g_player);
    if (!g_pc) { UE_LOGW("freecam: no controller"); return; }

    const ue_wrap::FVector loc = E::GetCameraLocation();
    const ue_wrap::FRotator rot = E::GetCameraRotation();
    void* camCls = R::FindClass(P::name::CameraActorClass);
    g_camActor = camCls ? E::SpawnActor(camCls, loc) : nullptr;
    if (!g_camActor) { UE_LOGE("freecam: failed to spawn CameraActor"); return; }
    E::SetActorRotation(g_camActor, rot);

    // Copy the player camera's post-process onto the freecam camera, else the view
    // renders with default PP and goes dark (lost exposure/grading). Zero the one
    // TArray (WeightedBlendables) in the copy so the cameras don't alias its heap.
    void* playerCam = ReadPtr(g_player, P::off::AmainPlayer_Camera);
    void* freecamCam = ReadPtr(g_camActor, P::off::ACameraActor_CameraComponent);
    if (playerCam && freecamCam) {
        uint8_t* dst = reinterpret_cast<uint8_t*>(freecamCam) + P::off::UCameraComponent_PostProcessSettings;
        uint8_t* src = reinterpret_cast<uint8_t*>(playerCam) + P::off::UCameraComponent_PostProcessSettings;
        std::memcpy(dst, src, P::off::FPostProcessSettings_Size);
        std::memset(dst + P::off::FPostProcessSettings_WeightedBlendables, 0, 0x10);  // don't alias the player's blendables
        const float bw = *reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(playerCam) + P::off::UCameraComponent_PostProcessBlendWeight);
        *reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(freecamCam) + P::off::UCameraComponent_PostProcessBlendWeight) = bw;
        UE_LOGI("freecam: copied player camera post-process (blendWeight=%.2f)", bw);
    } else {
        UE_LOGW("freecam: could not read camera components (player=%p freecam=%p) -- view may be dark",
                playerCam, freecamCam);
    }

    E::SetViewTargetWithBlend(g_pc, g_camActor, 0.15f);

    // The fly keys (WASD/Space/Ctrl) must not ALSO drive the pawn (user 2026-07-05).
    FreezePlayerControl();

    g_camPos = loc;
    g_lastMs.store(::GetTickCount64());
    g_active.store(true);
    UE_LOGI("freecam: ON at (%.0f,%.0f,%.0f)", loc.X, loc.Y, loc.Z);
}

void Disable() {
    g_active.store(false);
    UnfreezePlayerControl();
    if (g_pc && g_player && R::IsLive(g_pc) && R::IsLive(g_player))
        E::SetViewTargetWithBlend(g_pc, g_player, 0.15f);
    if (g_camActor && R::IsLive(g_camActor)) E::DestroyActor(g_camActor);
    g_camActor = nullptr;
    UE_LOGI("freecam: OFF");
}

void Teleport() {
    if (!g_active.load() || !g_player || !R::IsLive(g_player)) return;
    E::SetActorLocation(g_player, g_camPos);
    UE_LOGI("freecam: teleported player to (%.0f,%.0f,%.0f)", g_camPos.X, g_camPos.Y, g_camPos.Z);
}

// Per-tick movement, throttled (~150 Hz) off the ProcessEvent stream so it's
// frame-synced and dt-scaled (smooth regardless of fps), independent of any
// specific event firing. Runs on the game thread inside the detour.
void MovementTick() {
    if (!g_active.load() || !g_camActor || !g_pc) return;
    // Live role gate: a freecam activated BEFORE joining (or during hosting,
    // then the role changed) dies the moment this peer is a connected client.
    // Runs on the game thread -- Disable() restores the view target directly.
    if (!::coop::dev_gate::Allowed()) {
        UE_LOGW("freecam: auto-off -- dev features are disabled while connected as a client");
        Disable();
        return;
    }
    // Foreground-window gate (same reason as the hotkey thread): the WASD /
    // Space / Ctrl / Shift KeyDown reads are GLOBAL, so without this the
    // freecam would still move when the user types in the OTHER instance's
    // window. Stop moving immediately when our window loses focus. Also stop
    // while OUR overlay is capturing typed text (2026-07-09): WASD typed into the
    // chat/rebind field must not ALSO fly the camera.
    if (!::coop::ini_config::IsOurWindowForeground() ||
        ::coop::ini_config::IsOverlayCapturingText()) return;
    // The level may have reloaded under us (the cached actors are then freed).
    // Bail without touching dead objects -- never SetActorLocation a freed actor.
    if (!R::IsLive(g_camActor) || !R::IsLive(g_pc)) {
        g_active.store(false);
        UnfreezePlayerControl();  // restores if the pawn survived; no-ops on a freed pawn
        g_camActor = nullptr; g_pc = nullptr; g_player = nullptr;
        UE_LOGW("freecam: view target invalidated (level change?); auto-off");
        return;
    }
    const unsigned long long now = ::GetTickCount64();
    const unsigned long long last = g_lastMs.load();
    if (now - last < 6) return;  // throttle; also blocks re-entrant UFunction calls
    g_lastMs.store(now);
    float dt = static_cast<float>(now - last) / 1000.f;
    if (dt > 0.05f) dt = 0.05f;  // clamp hitches

    // Mouse wheel adjusts fly speed (each notch ~+/-15%).
    if (int notches = g_wheelNotches.exchange(0)) {
        g_speed *= std::pow(1.15f, static_cast<float>(notches));
        if (g_speed < kMinSpeed) g_speed = kMinSpeed;
        if (g_speed > kMaxSpeed) g_speed = kMaxSpeed;
    }

    // Look = the game's own control rotation (smooth, no raw mouse).
    const ue_wrap::FRotator rot = E::GetControlRotation(g_pc);
    const float yaw = rot.Yaw * kDeg2Rad;
    const float pitch = rot.Pitch * kDeg2Rad;
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const ue_wrap::FVector fwd{cp * cy, cp * sy, sp};
    const ue_wrap::FVector right{-sy, cy, 0.f};

    float mx = 0, my = 0, mz = 0;
    if (KeyDown('W')) { mx += fwd.X; my += fwd.Y; mz += fwd.Z; }
    if (KeyDown('S')) { mx -= fwd.X; my -= fwd.Y; mz -= fwd.Z; }
    if (KeyDown('D')) { mx += right.X; my += right.Y; }
    if (KeyDown('A')) { mx -= right.X; my -= right.Y; }
    if (KeyDown(VK_SPACE)) mz += 1.f;
    if (KeyDown(VK_CONTROL)) mz -= 1.f;

    const float len = std::sqrt(mx * mx + my * my + mz * mz);
    if (len > 0.001f) {
        const float speed = (KeyDown(VK_SHIFT) ? g_speed * kFastMul : g_speed) * dt / len;
        g_camPos.X += mx * speed;
        g_camPos.Y += my * speed;
        g_camPos.Z += mz * speed;
        E::SetActorLocation(g_camActor, g_camPos);
    }
    // Always keep the camera looking where the player aims (smooth game look).
    E::SetActorRotation(g_camActor, rot);
}

// Low-level mouse hook to read the wheel (GetAsyncKeyState can't -- the wheel is
// event-based). Global, but the proc is trivial (one atomic add when active) and
// never consumes the event. Requires a message pump on its thread.
LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    // Only accrue wheel notches when freecam is on AND our window is focused
    // -- else the wheel in another window (e.g. the other VOTV instance) would
    // change THIS instance's freecam speed. Foreground check is cheap.
    if (code == HC_ACTION && wParam == WM_MOUSEWHEEL && g_active.load() &&
        ::coop::ini_config::IsOurWindowForeground()) {
        const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        const int delta = GET_WHEEL_DELTA_WPARAM(ms->mouseData);  // multiples of 120
        if (delta) g_wheelNotches.fetch_add(delta / WHEEL_DELTA);
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

// Audit H11 (2026-05-27): wheel hook thread ID, captured at thread entry.
// HotkeyThread observes shutdown + posts WM_QUIT here so GetMessageW unblocks
// and UnhookWindowsHookEx runs cleanly. Pre-fix the thread blocked in
// GetMessageW until process termination; Windows force-killed it without
// running the unhook (LL mouse hooks left registered for ~30s in winhk
// timeout, which can briefly delay subsequent VOTV launches).
std::atomic<DWORD> g_wheelHookTid{0};

DWORD WINAPI WheelHookThread(LPVOID) {
    g_wheelHookTid.store(::GetCurrentThreadId(), std::memory_order_release);
    HHOOK hook = ::SetWindowsHookExW(WH_MOUSE_LL, &MouseProc, ::GetModuleHandleW(nullptr), 0);
    if (!hook) {
        UE_LOGW("freecam: wheel hook install failed; speed-by-wheel disabled");
        g_wheelHookTid.store(0, std::memory_order_release);
        return 0;
    }
    MSG msg;  // LL hooks dispatch on this thread's message queue -> must pump
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    ::UnhookWindowsHookEx(hook);
    g_wheelHookTid.store(0, std::memory_order_release);
    return 0;
}

// Input/movement driver. Polls HOME (toggle -- kept by user request alongside the
// menu checkbox), the in-freecam controls (MMB bring-player), and drives the
// per-frame MovementTick while active.
DWORD WINAPI InputDriverThread(LPVOID) {
    bool prevHome = false, prevMmb = false;
    while (!coop::shutdown::IsShuttingDown()) {
        // Foreground-window gate: GetAsyncKeyState is GLOBAL across processes, so
        // HOME/WASD/MMB in the client's window would otherwise drive THIS instance's
        // freecam. Only react to keys when OUR window is focused.
        const bool focused = ::coop::ini_config::IsOurWindowForeground();

        const bool home = focused && KeyDown(VK_HOME);
        if (home && !prevHome) {
            GT::Post([] { g_active.load() ? Disable() : Enable(); });
        }
        prevHome = home;

        const bool mmb = focused && KeyDown(VK_MBUTTON);
        if (mmb && !prevMmb && g_active.load()) {
            GT::Post([] { Teleport(); });
        }
        prevMmb = mmb;

        // Drive movement on the GAME THREAD via Post (never touch UFunctions from
        // this worker thread). Collapse to AT MOST ONE queued tick: if the game is
        // slower than this 60 Hz loop, posting unconditionally would back up the
        // queue (each task grabs the pump mutex). The pending flag keeps exactly one
        // in flight; the tick clears it. dt-scaling keeps motion correct.
        if (g_active.load() && !g_movePosted.exchange(true)) {
            GT::Post([] { g_movePosted.store(false); MovementTick(); });
        }

        ::Sleep(16);  // 60 Hz (user-set 2026-06-04, was 125)
    }
    // Audit H11 (2026-05-27): shutdown observed; wake the wheel hook thread.
    // It's blocked in GetMessageW; without WM_QUIT it would hang until process
    // termination, denying UnhookWindowsHookEx a clean exit.
    if (DWORD tid = g_wheelHookTid.load(std::memory_order_acquire)) {
        ::PostThreadMessageW(tid, WM_QUIT, 0, 0);
    }
    return 0;
}

// Start the driver + wheel-hook threads once, lazily, on the first activation (no
// idle worker thread / system-wide LL mouse hook for players who never freecam).
void EnsureThreads() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    if (HANDLE t = ::CreateThread(nullptr, 0, &InputDriverThread, nullptr, 0, nullptr)) {
        ::CloseHandle(t);  // detached
    }
    if (HANDLE t = ::CreateThread(nullptr, 0, &WheelHookThread, nullptr, 0, nullptr)) {
        ::CloseHandle(t);
    }
    UE_LOGI("freecam: driver threads started (WASD/Space/Ctrl=move, Shift=fast, wheel=speed, MMB=bring player)");
}

}  // namespace

void Init() {
    // Master kill-switch first: [dev] enabled=0 forces every dev feature off.
    if (!::coop::ini_config::MasterEnabled()) {
        UE_LOGI("freecam: disabled by master switch ([dev] enabled=0)");
        return;
    }
    if (!::coop::ini_config::IsIniKeyTrue("freecam")) {
        UE_LOGI("freecam: HOME toggle off at boot (set [dev] freecam=1 to enable it; the "
                "F1 menu still toggles freecam under [dev] devkeys)");
        return;
    }
    EnsureThreads();  // start HOME polling + movement driver + wheel hook now
    UE_LOGI("freecam: ENABLED at boot (HOME=toggle, WASD/Space/Ctrl=move, Shift=fast, "
            "wheel=speed, MMB=bring player; also in the F1 menu)");
}

void SetActive(bool on) {
    EnsureThreads();
    GT::Post([on] {
        if (on && !g_active.load())       Enable();
        else if (!on && g_active.load())  Disable();
    });
}

bool IsActive() { return g_active.load(std::memory_order_acquire); }

}  // namespace coop::dev::freecam
