// ui/overlay_backend_internal.h -- TU-private seam between the overlay-backend
// dispatcher (overlay_backend.cpp) and the per-RHI implementations
// (overlay_backend_dx11.cpp / overlay_backend_dx12.cpp). Nothing outside
// src/ui/overlay_backend*.cpp may include this (the public surface is
// include/ui/overlay_backend.h). Same pattern as coop/config's
// config_internal.h.

#pragma once

#include <cstdint>
#include <vector>

struct IDXGISwapChain;

namespace ui::overlay_backend::detail {

// WIC decode of a PNG/BMP/JPG file to 32bpp BGRA. RHI-agnostic (both backends
// upload the same pixels their own way). false on any failure.
bool DecodeImageFileBgra(const wchar_t* path, std::vector<uint8_t>& outBgra,
                         unsigned& outW, unsigned& outH);

}  // namespace ui::overlay_backend::detail

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

}  // namespace ui::overlay_backend::dx11

namespace ui::overlay_backend::dx12 {

bool Live();

// Bring-up step 1. Detects D3D12 off the swapchain (P1/P3 logged once), arms
// the ExecuteCommandLists capture hook, and runs candidate+confirmation on the
// presenting queue. Returns true ONLY when the queue is confirmed -- until
// then the overlay stays down and Present passes through (a handful of frames).
bool CaptureDevice(IDXGISwapChain* sc);
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

}  // namespace ui::overlay_backend::dx12

// Which command queue presents the swapchain (overlay_backend_dx12_capture.cpp).
// D3D12 has no API for it; see that TU's header comment for the measurement.
struct ID3D12Device;
struct ID3D12CommandQueue;

namespace ui::overlay_backend::dx12_capture {

// The D3D12 device off the swapchain (owned here; borrowed by the renderer).
ID3D12Device* Device();

// Per-present: detect D3D12 once, arm the capture hook, run
// candidate+confirmation. Returns the presenting queue ONLY once confirmed
// (nullptr while pending, and forever after a HALT).
ID3D12CommandQueue* TryConfirmQueue(IDXGISwapChain* sc);

// Swapchain-creation timing probe, installed at boot from imgui_overlay::Init:
// hooks IDXGIFactory::CreateSwapChain(+ForHwnd) to record whether our boot
// precedes the game's swapchain creation. Measured 2026-07-26: it does NOT on
// this rig, which is why the ECL capture (timing-independent) is the shipping
// mechanism and the factory route was never built into prod.
void InstallCreationProbe();

// Re-arm after a swapchain recreation: the "this queue presents that chain"
// fact no longer holds, so require a fresh confirmation (the previously
// confirmed queue is seeded as the candidate).
void Rearm();

// Disarm the capture hooks + release the device ref.

}  // namespace ui::overlay_backend::dx12_capture
