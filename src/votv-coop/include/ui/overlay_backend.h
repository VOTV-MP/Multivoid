// ui/overlay_backend.h -- the RHI-specific half of the ImGui overlay.
//
// imgui_overlay.cpp owns the hooks, the WndProc, and surface compositing (the
// platform layer); this interface owns everything that touches a concrete D3D
// device: device capture off the game's swapchain, the ImGui renderer backend,
// the render target, and texture creation for UI surfaces. One implementation
// per RHI -- overlay_backend_dx11.cpp today, overlay_backend_dx12.cpp for the
// D3D12 RHI -- selected at first present by what the game's swapchain actually
// is. No RHI types cross this boundary.
//
// Design of record:
// research/findings/tooling/votv-imgui-dx12-overlay-DESIGN-2026-07-26.md

#pragma once

struct IDXGISwapChain;

namespace ui::overlay_backend {

// Which renderer is live -- "DX11" / "DX12" once bring-up committed, nullptr
// while pending. Feeds the RHI indicator (F1 menu line + the bring-up log).
const char* Kind();

// First-present bring-up, step 1: capture the device/context off the game's
// swapchain. false = this swapchain is not this backend's RHI (or capture
// failed) -- the overlay stays down and Present passes through.
bool CaptureDevice(IDXGISwapChain* sc);

// Unwind arm for a failure between CaptureDevice and InitRenderer: release
// everything CaptureDevice acquired.
void AbandonCapture();

// Bring-up, step 2 (ImGui context + Win32 backend are live): init the ImGui
// renderer backend and create the render target. false = caller unwinds.
bool InitRenderer(IDXGISwapChain* sc);

// Per-frame, render thread, BEFORE ImGui::NewFrame.
void NewFrame();

// Drop the renderer's device objects so the next NewFrame lazily re-creates
// them (the font-atlas rescale path).
void InvalidateDeviceObjects();

// Re-create the render target if a resize dropped it (cheap no-op otherwise).
void EnsureTarget(IDXGISwapChain* sc);

// Draw ImGui's current draw data into the swapchain's backbuffer.
void RenderDrawData(IDXGISwapChain* sc);

// ResizeBuffers bracket: release backbuffer-derived state BEFORE the game's
// resize runs; re-derive it after a successful resize.
void OnResizeRelease();
void OnResizeRecreate(IDXGISwapChain* sc);

// Decode an image file (PNG/BMP/JPG via WIC) into a device texture usable as
// an ImTextureID. Render thread only. The caller caches the result and MUST
// return it through DestroyTexture when done (a dropped id leaks the texture).
void* CreateTextureFromImageFile(const wchar_t* path, int* outW, int* outH);

// Release a texture returned by CreateTextureFromImageFile. Render thread
// only. DX11: releases the SRV (the texture rides its refcount). null is a
// no-op.
void DestroyTexture(void* id);

// Install the swapchain-creation timing probe (the DX12 stage-1 measurement:
// does our boot precede the game's swapchain creation, and what queue arrives
// as CreateSwapChain*'s pDevice). Called once from imgui_overlay::Init;
// log-only, no behavior change on any RHI.
void InstallCreationProbe();

// Tear down everything this backend owns. rendererWasLive mirrors the
// overlay's g_imguiReady latch (renderer-backend Shutdown only if Init ran).

}  // namespace ui::overlay_backend
