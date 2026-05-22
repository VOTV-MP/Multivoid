#include "dev/freecam.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace dev::freecam {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

namespace {

bool g_enabled = false;                 // from ini (dev switch)
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

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path);
    const size_t sep = p.find_last_of(L"\\/");
    return sep == std::wstring::npos ? L"." : p.substr(0, sep);
}

// Minimal ini read: look for a line "freecam=1" / "freecam = true" (case/space
// tolerant) in votv-coop.ini next to the DLL.
bool ReadIniEnabled() {
    const std::wstring path = ModuleDir() + L"\\votv-coop.ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) return false;
    char line[128];
    bool on = false;
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        // strip spaces
        s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }), s.end());
        for (auto& c : s) c = static_cast<char>(::tolower(c));
        if (s == "freecam=1" || s == "freecam=true") { on = true; break; }
        if (s == "freecam=0" || s == "freecam=false") { on = false; break; }
    }
    std::fclose(f);
    return on;
}

void Enable() {
    g_player = R::FindObjectByClass(P::name::MainPlayerClass);
    if (!g_player) { UE_LOGW("freecam: no local player"); return; }
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

    g_camPos = loc;
    g_lastMs.store(::GetTickCount64());
    g_active.store(true);
    UE_LOGI("freecam: ON at (%.0f,%.0f,%.0f)", loc.X, loc.Y, loc.Z);
}

void Disable() {
    g_active.store(false);
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
    // The level may have reloaded under us (the cached actors are then freed).
    // Bail without touching dead objects -- never SetActorLocation a freed actor.
    if (!R::IsLive(g_camActor) || !R::IsLive(g_pc)) {
        g_active.store(false);
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
    if (code == HC_ACTION && wParam == WM_MOUSEWHEEL && g_active.load()) {
        const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        const int delta = GET_WHEEL_DELTA_WPARAM(ms->mouseData);  // multiples of 120
        if (delta) g_wheelNotches.fetch_add(delta / WHEEL_DELTA);
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

DWORD WINAPI WheelHookThread(LPVOID) {
    HHOOK hook = ::SetWindowsHookExW(WH_MOUSE_LL, &MouseProc, ::GetModuleHandleW(nullptr), 0);
    if (!hook) { UE_LOGW("freecam: wheel hook install failed; speed-by-wheel disabled"); return 0; }
    MSG msg;  // LL hooks dispatch on this thread's message queue -> must pump
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    ::UnhookWindowsHookEx(hook);
    return 0;
}

DWORD WINAPI HotkeyThread(LPVOID) {
    bool prevHome = false, prevMmb = false;
    for (;;) {
        const bool home = KeyDown(VK_HOME);
        if (home && !prevHome) {
            GT::Post([] { g_active.load() ? Disable() : Enable(); });
        }
        prevHome = home;

        const bool mmb = KeyDown(VK_MBUTTON);
        if (mmb && !prevMmb && g_active.load()) {
            GT::Post([] { Teleport(); });
        }
        prevMmb = mmb;

        // Drive movement on the GAME THREAD via Post (never touch UFunctions from
        // this worker thread). Collapse to AT MOST ONE queued tick: if the game is
        // slower than this 125 Hz loop, posting unconditionally would back up the
        // queue (each task grabs the pump mutex). The pending flag keeps exactly one
        // in flight; the tick clears it. dt-scaling keeps motion correct.
        if (g_active.load() && !g_movePosted.exchange(true)) {
            GT::Post([] { g_movePosted.store(false); MovementTick(); });
        }

        ::Sleep(8);
    }
}

}  // namespace

void Init() {
    g_enabled = ReadIniEnabled();
    if (!g_enabled) {
        UE_LOGI("freecam: disabled (set [dev] freecam=1 in votv-coop.ini to enable)");
        return;
    }
    if (HANDLE t = ::CreateThread(nullptr, 0, &HotkeyThread, nullptr, 0, nullptr)) {
        ::CloseHandle(t);  // detached; close the handle so it doesn't leak
    }
    if (HANDLE t = ::CreateThread(nullptr, 0, &WheelHookThread, nullptr, 0, nullptr)) {
        ::CloseHandle(t);
    }
    UE_LOGI("freecam: ENABLED (HOME=toggle, WASD/Space/Ctrl=move, Shift=fast, wheel=speed, MMB=teleport)");
}

}  // namespace dev::freecam
