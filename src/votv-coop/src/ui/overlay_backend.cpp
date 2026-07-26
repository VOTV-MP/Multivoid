// ui/overlay_backend.cpp -- the RHI dispatcher behind ui/overlay_backend.h.
//
// Owns exactly one concept: WHICH per-RHI implementation is live. The first
// present's CaptureDevice picks it by what the game's swapchain actually is:
// DX11 capture succeeds -> the DX11 backend is active; otherwise the DX12
// stage-1 instrumentation ticks (capture + tally; the DX12 renderer is
// commit 3 of the design of record,
// research/findings/tooling/votv-imgui-dx12-overlay-DESIGN-2026-07-26.md).

#include "ui/overlay_backend.h"

#include "overlay_backend_internal.h"

namespace ui::overlay_backend {
namespace {

enum class Active { None, Dx11 };
Active g_active = Active::None;  // render-thread-owned (all writers run in the Present detour)

}  // namespace

const char* Kind() {
    return (g_active == Active::Dx11 && dx11::Live()) ? "DX11" : nullptr;
}

bool CaptureDevice(IDXGISwapChain* sc) {
    if (dx11::CaptureDevice(sc)) {
        g_active = Active::Dx11;
        return true;
    }
    dx12::PendingTick(sc);  // not DX11: run the stage-1 D3D12 instrumentation
    return false;
}

void AbandonCapture() {
    if (g_active == Active::Dx11) {
        dx11::AbandonCapture();
        g_active = Active::None;
    }
}

bool InitRenderer(IDXGISwapChain* sc) {
    return g_active == Active::Dx11 && dx11::InitRenderer(sc);
}

void NewFrame() {
    if (g_active == Active::Dx11) dx11::NewFrame();
}

void InvalidateDeviceObjects() {
    if (g_active == Active::Dx11) dx11::InvalidateDeviceObjects();
}

void EnsureTarget(IDXGISwapChain* sc) {
    if (g_active == Active::Dx11) dx11::EnsureTarget(sc);
}

void RenderDrawData(IDXGISwapChain* sc) {
    if (g_active == Active::Dx11) dx11::RenderDrawData(sc);
}

void OnResizeRelease() {
    if (g_active == Active::Dx11) dx11::OnResizeRelease();
}

void OnResizeRecreate(IDXGISwapChain* sc) {
    if (g_active == Active::Dx11) dx11::OnResizeRecreate(sc);
}

void* CreateTextureFromImageFile(const wchar_t* path, int* outW, int* outH) {
    if (g_active == Active::Dx11) return dx11::CreateTextureFromImageFile(path, outW, outH);
    return nullptr;
}

void DestroyTexture(void* id) {
    if (g_active == Active::Dx11) dx11::DestroyTexture(id);
}

void InstallCreationProbe() { dx12::InstallCreationProbe(); }

void Shutdown(bool rendererWasLive) {
    if (g_active == Active::Dx11) dx11::Shutdown(rendererWasLive);
    dx12::Stage1Shutdown();
    g_active = Active::None;
}

}  // namespace ui::overlay_backend
