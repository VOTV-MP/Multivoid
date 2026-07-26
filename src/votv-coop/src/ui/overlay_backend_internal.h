// ui/overlay_backend_internal.h -- TU-private seam between the overlay-backend
// dispatcher (overlay_backend.cpp) and the per-RHI implementations
// (overlay_backend_dx11.cpp / overlay_backend_dx12.cpp). Nothing outside
// src/ui/overlay_backend*.cpp may include this (the public surface is
// include/ui/overlay_backend.h). Same pattern as coop/config's
// config_internal.h.

#pragma once

struct IDXGISwapChain;

namespace ui::overlay_backend::dx11 {

bool Live();
bool CaptureDevice(IDXGISwapChain* sc);   // silent on not-DX11 (dispatcher owns the RHI verdict)
void AbandonCapture();
bool InitRenderer(IDXGISwapChain* sc);
void NewFrame();
void InvalidateDeviceObjects();
void EnsureTarget(IDXGISwapChain* sc);
void RenderDrawData(IDXGISwapChain* sc);
void OnResizeRelease();
void OnResizeRecreate(IDXGISwapChain* sc);
void* CreateTextureFromImageFile(const wchar_t* path, int* outW, int* outH);
void DestroyTexture(void* id);
void Shutdown(bool rendererWasLive);

}  // namespace ui::overlay_backend::dx11

namespace ui::overlay_backend::dx12 {

// Stage 1 (capture instrumentation; the renderer is commit 3 of the design).
// Called from imgui_overlay::Init BEFORE the game creates its swapchain (on
// the rig; the timing is exactly what this probe measures): hooks
// IDXGIFactory2::CreateSwapChain(ForHwnd) to log whether creation-precede
// holds and what pDevice (the presenting queue on D3D12) arrives.
void InstallCreationProbe();

// Per-present tick while the overlay is NOT live and the swapchain is not
// DX11: detects D3D12 (P1/P3 hr logged), installs the ExecuteCommandLists
// capture hook, runs the queue tally, flushes the stage-1 summary at the
// OFF-edge and Disables the hooks. Never claims the overlay.
void PendingTick(IDXGISwapChain* sc);

// Mod shutdown: disable stage-1 hooks + release the probe's device ref.
void Stage1Shutdown();

}  // namespace ui::overlay_backend::dx12
