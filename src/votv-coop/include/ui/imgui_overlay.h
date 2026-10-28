// ui/imgui_overlay.h -- Dear ImGui render host over the game's DXGI swapchain.
//
// The ONE ImGui integration point for the whole mod (RULE: dev features live in a
// categorized ImGui menu; future: a main-menu MP server browser on this same host).
// It hooks the engine's present and resize paths (via our MinHook substrate), auto-detects the RHI
// at the swapchain (DX11, DX12 and Vulkan draw -- the per-RHI half lives behind ui/overlay_backend.h),
// runs the ImGui frame, and calls the active UI surface's Render(). F1 toggles visibility. Input is
// captured only while visible. The window's messages are routed in by coop/input/game_window_proc
// through HandleWindowMessage.
//
// Principle 7: pure engine substrate + presentation -- NO gameplay/network logic.
// The UI surfaces (ui::dev_menu, future server browser) own their content; the
// overlay only hosts them.
//
// Standalone (RULE 3): OUR vendored ImGui (third_party/imgui), never UE4SS's.

#pragma once

#include <cstdint>

#include <string>

namespace ui::imgui_overlay {

// Install the present/resize hooks + bring up ImGui lazily on the
// first real Present (so the game's device + window already exist). Idempotent;
// safe to call once from harness boot. Returns false if the present-hook target
// couldn't be resolved (logged); the caller may retry. The heavy init (ImGui
// context, backend, RTV) happens inside the Present detour on the
// render thread the first time the game presents a frame.
bool Init();

// The overlay's share of one game-window message: the F1, tilde, T, V and ESC hotkeys and the
// ImGui input feed and capture swallow (all only once ImGui is up), and the native text field's
// keys. True means the message is consumed and `result` is the window procedure's return; false
// means it goes on to the next consumer and the game. Called on every message from the window
// procedure's thread, ahead of the shutdown handling and the game; needs no present.
// The parameters are the window procedure's (HWND, UINT, WPARAM, LPARAM, LRESULT) in plain integer
// types, so this header stays free of the Windows headers.
bool HandleWindowMessage(void* hwnd, unsigned msg, uintptr_t wParam, intptr_t lParam,
                         intptr_t& result);

// Called with every readiness milestone the rig is told (coop/session/rig_ready.h). The first one that
// means a gameplay world is up (hosting, world-ready, solo-world) judges the draw seams: none of the three
// present seams having fired by then is reported once, and the scan ledger's draw row is marked missed.
// Any thread.
void OnMilestone(const char* milestone);

// Force visibility (e.g. a future main-menu button opening the server browser).
void SetVisible(bool visible);

// TEST-ONLY (VOTVCOOP_SCOREBOARD_OPEN via ui/overlay_test_arm.cpp): start the
// player-list scoreboard visible through the FORCED latch, which survives the
// host window losing focus to the launching client window (the WM_KILLFOCUS
// reset only clears the real tilde-key state). No un-force exists on purpose --
// the latch lives for the screenshot run.
void ForceScoreboardOpen();

// WHICH SURFACES CURRENTLY CLAIM THE MOUSE, as a short list of names ("none" if the
// answer is nobody). Diagnostic only.
//
// While ANY of them is up, `CaptureActive()` is true, and that has two consequences a
// reader of this header will not guess: HandleWindowMessage swallows every mouse message before
// the game sees it, and the SetCursorPos detour returns TRUE without moving the pointer.
// So a surface the mod draws OUTSIDE ImGui -- the native server browser, the native host
// window -- silently receives no mouse input at all whenever an ImGui surface is open at
// the same time, and any code that positions the cursor gets a success it did not get.
// That combination cost a long hunt: three lab defects, an unexplained close button, and
// a "the button is not hit-testable" conclusion that was really "the pointer never moved".
// This exists so the next reading is one log line rather than a bisect.
std::string CaptureOwners();


}  // namespace ui::imgui_overlay
