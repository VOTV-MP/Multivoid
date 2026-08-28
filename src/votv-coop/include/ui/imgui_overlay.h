// ui/imgui_overlay.h -- Dear ImGui render host over the game's DXGI swapchain.
//
// The ONE ImGui integration point for the whole mod (RULE: dev features live in a
// categorized ImGui menu; future: a main-menu MP server browser on this same host).
// It hooks IDXGISwapChain::Present (via our MinHook substrate) + ResizeBuffers + the
// window's WndProc, auto-detects the RHI at the swapchain (DX11 and DX12 both
// draw -- the per-RHI half lives behind ui/overlay_backend.h), runs the ImGui
// frame, and calls the active UI surface's Render(). F1 toggles visibility.
// Input is captured only while visible.
//
// Principle 7: pure engine substrate + presentation -- NO gameplay/network logic.
// The UI surfaces (ui::dev_menu, future server browser) own their content; the
// overlay only hosts them.
//
// Standalone (RULE 3): OUR vendored ImGui (third_party/imgui), never UE4SS's.

#pragma once

namespace ui::imgui_overlay {

// Install the DXGI present/resize/wndproc hooks + bring up ImGui lazily on the
// first real Present (so the game's device + window already exist). Idempotent;
// safe to call once from harness boot. Returns false if the present-hook target
// couldn't be resolved (logged); the caller may retry. The heavy init (ImGui
// context, backend, RTV, wndproc swap) happens inside the Present detour on the
// render thread the first time the game presents a frame.
bool Init();

// True while the menu is shown (F1 toggles it). Other systems can read this to,
// e.g., pause game input. Lock-free.
bool IsVisible();

// Force visibility (e.g. a future main-menu button opening the server browser).
void SetVisible(bool visible);

// TEST-ONLY (VOTVCOOP_SCOREBOARD_OPEN via ui/overlay_test_arm.cpp): start the
// player-list scoreboard visible through the FORCED latch, which survives the
// host window losing focus to the launching client window (the WM_KILLFOCUS
// reset only clears the real tilde-key state). No un-force exists on purpose --
// the latch lives for the screenshot run.
void ForceScoreboardOpen();

// Image-file -> ImTextureID decoding lives in ui/overlay_backend.h
// (overlay_backend::CreateTextureFromImageFile) -- it is RHI-specific.


}  // namespace ui::imgui_overlay
