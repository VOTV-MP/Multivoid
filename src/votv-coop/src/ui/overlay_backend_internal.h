// ui/overlay_backend_internal.h -- TU-private seam between the overlay-backend
// dispatcher (overlay_backend.cpp), the per-RHI implementations
// (overlay_backend_dx11.cpp / overlay_backend_dx12.cpp) and the two callers that build a
// FrameTarget (overlay_seams.cpp, imgui_overlay.cpp). Nothing else may include this (the
// public surface is include/ui/overlay_backend.h). Same pattern as coop/config's
// config_internal.h.

#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

struct IDXGISwapChain;
struct ImTextureRef;

namespace ui::overlay_backend {

// Which RHI is presenting; set by the engine seam that fired.
enum class Rhi : uint8_t { None, Dx11, Dx12, Vulkan };

// What the Vulkan seam hands one overlay frame; it lives on the stack of whoever fills it. `viewport` is
// the FVulkanViewport* the engine's Present received. `presentInfo` is the VkPresentInfoKHR of the
// swapped vkQueuePresentKHR (null at bring-up). `queue` is the engine's FVulkanQueue* (Present's fifth
// argument) at bring-up and the present's VkQueue in a frame. `imageIndex` is the image the present
// shows. `waitOut` is written by the backend's render with the VkSemaphore the present must wait on
// instead of the engine's own (null: the present goes through untouched).
struct VulkanFrame {
    void*    viewport;
    void*    presentInfo;
    void*    queue;
    uint32_t imageIndex;
    void*    waitOut;
};

// What one overlay frame draws into. The DX seams fill dxgi, and hwnd only until the overlay is up
// (the bring-up is its one reader); Vulkan fills vk.
struct FrameTarget {
    Rhi            rhi;
    HWND           hwnd;
    IDXGISwapChain* dxgi;
    VulkanFrame*   vk;
};

// One table per RHI, every member set except where a half has no meaning for it (the
// resize pair belongs to the DX resize bracket; the dispatcher null-checks it).
struct BackendOps {
    bool  (*capture)(const FrameTarget&);
    void  (*abandon)();
    bool  (*init)(const FrameTarget&);
    void  (*newFrame)();
    void  (*ensureTarget)(const FrameTarget&);
    void  (*render)(const FrameTarget&);
    void  (*invalidate)();
    void  (*resizeRelease)();
    void  (*resizeRecreate)(const FrameTarget&);
    void* (*textureFromFile)(const wchar_t*, int*, int*);
    void  (*destroyTexture)(void*);
    const char* name;
    // Null: the handle textureFromFile returned is the ImTextureID itself (the DX backends).
    ImTextureRef (*textureRef)(void*);
};

namespace detail {

// WIC decode of a PNG/BMP/JPG file to 32bpp BGRA. RHI-agnostic (both backends
// upload the same pixels their own way). false on any failure.
bool DecodeImageFileBgra(const wchar_t* path, std::vector<uint8_t>& outBgra,
                         unsigned& outW, unsigned& outH);

}  // namespace detail

}  // namespace ui::overlay_backend

namespace ui::overlay_backend::dx11 {
const BackendOps& Ops();
}  // namespace ui::overlay_backend::dx11

namespace ui::overlay_backend::dx12 {
const BackendOps& Ops();
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

// Swapchain-creation timing probe, installed at boot from imgui_overlay::Init: hooks
// IDXGIFactory::CreateSwapChain (and ForHwnd) to record whether our boot precedes the game's
// swapchain creation. It does not on the rig this was measured on, which is why the ECL
// capture -- timing-independent -- is the shipping mechanism and the factory route was never
// built into production.
void InstallCreationProbe();

// Re-arm after a swapchain recreation: the "this queue presents that chain"
// fact no longer holds, so require a fresh confirmation (the previously
// confirmed queue is seeded as the candidate).
void Rearm();

// Disarm the capture hooks + release the device ref.

}  // namespace ui::overlay_backend::dx12_capture
