// ui/overlay_backend_dx11.cpp -- the D3D11 implementation of ui/overlay_backend.h.
//
// g_device and g_context are committed in CaptureDevice, before the renderer half of bring-up
// has run, and the AbandonCapture unwind arm is what undoes them on a later failure. That is
// safe here and only here: bring-up is single-threaded on the render thread, and nothing reads
// those globals between the two steps.

#include "ui/overlay_backend.h"

#include "overlay_backend_internal.h"
#include "ue_wrap/core/log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstdint>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

namespace ui::overlay_backend::dx11 {
namespace {

ID3D11Device*           g_device  = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
ID3D11RenderTargetView* g_rtv     = nullptr;

void CreateRTV(IDXGISwapChain* sc) {
    if (g_rtv || !g_device) return;
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        const HRESULT hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        if (FAILED(hr)) UE_LOGE("imgui_overlay: CreateRenderTargetView failed (hr=0x%08lX) -- menu won't draw", hr);
        back->Release();
    } else {
        UE_LOGW("imgui_overlay: swapchain GetBuffer(0) failed -- no RTV this resize");
    }
}

void ReleaseRTV() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool CaptureDevice(const FrameTarget& t) {
    IDXGISwapChain* sc = t.dxgi;
    // The D3D11 present seam fired, so a swapchain without an ID3D11Device means the seam's AOB
    // landed on the wrong function. Nothing else decides the RHI now, so say so once: the
    // bring-up retries on every frame and would otherwise fail without a line.
    ID3D11Device* dev = nullptr;
    const HRESULT hr = sc->GetDevice(IID_PPV_ARGS(&dev));
    if (FAILED(hr) || !dev) {
        if (dev) dev->Release();
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            UE_LOGE("imgui_overlay: the FD3D11Viewport::PresentChecked seam fired but its swapchain "
                    "(%p) has no ID3D11Device (GetDevice hr=0x%08lX) -- the seam is on the wrong "
                    "function or the game is not D3D11; the overlay stays down",
                    static_cast<void*>(sc), static_cast<unsigned long>(hr));
            ue_wrap::log::Flush();
        }
        return false;
    }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    g_device = dev;
    g_context = ctx;
    return true;
}

void AbandonCapture() {
    // Total, not partial: this is the unwind arm for a capture that CaptureDevice committed, so it
    // must leave the backend as if the capture never happened. Today its two callers both run
    // before InitRenderer commits, but a render target left pointing at a released device -- or
    // a stale device pointer -- would be a use-after-free the moment one did not.
    ReleaseRTV();
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device)  { g_device->Release();  g_device = nullptr; }
}

bool InitRenderer(const FrameTarget& t) {
    if (!ImGui_ImplDX11_Init(g_device, g_context)) {
        UE_LOGE("imgui_overlay: ImGui_ImplDX11_Init failed");
        return false;
    }
    // THE FLAG STAYS ON, for both RHIs. Clearing ImGuiBackendFlags_RendererHasTextures here would
    // let DX11's dynamic atlas and DX12's legacy-stripped one ship TWO drawable repertoires from
    // ONE binary, chosen by the player's GPU API. DX12 runs on ImGui_ImplDX12_InitInfo with our own
    // descriptor allocator, so both RHIs service textures: one regime, one repertoire, whatever the
    // RHI.
    //
    // ONE AXIS, deliberately -- a per-RHI flip would re-create the exact defect this rule prevents,
    // and a DX12-only risk is answered by the atlas-regime census in CI, not by splitting the flag.
    CreateRTV(t.dxgi);
    return true;
}

void NewFrame() { ImGui_ImplDX11_NewFrame(); }

void InvalidateDeviceObjects() { ImGui_ImplDX11_InvalidateDeviceObjects(); }

void EnsureTarget(const FrameTarget& t) {
    if (!g_rtv) CreateRTV(t.dxgi);  // recreate after a resize
}

void RenderDrawData(const FrameTarget&) {
    if (g_rtv) {
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

void OnResizeRelease() { ReleaseRTV(); }

void OnResizeRecreate(const FrameTarget& t) { CreateRTV(t.dxgi); }

void* CreateTextureFromImageFile(const wchar_t* path, int* outW, int* outH) {
    // RENDER THREAD ONLY (call from a surface's Render() -- the Present detour
    // thread that owns g_device). The WIC decode is the RHI-agnostic shared
    // helper (overlay_backend.cpp); this half is the D3D11 upload: an
    // immutable texture + SRV. The returned SRV is the ImTextureID; the caller
    // caches it and returns it via DestroyTexture.
    if (!g_device || !path) return nullptr;
    std::vector<uint8_t> px;
    unsigned w = 0, h = 0;
    if (!detail::DecodeImageFileBgra(path, px, w, h)) return nullptr;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px.data(), w * 4u, 0};
    if (FAILED(g_device->CreateTexture2D(&td, &sd, &tex)) || !tex) return nullptr;
    g_device->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();  // the SRV holds its own reference
    if (srv) {
        if (outW) *outW = static_cast<int>(w);
        if (outH) *outH = static_cast<int>(h);
    }
    return srv;
}

void DestroyTexture(void* id) {
    if (id) static_cast<ID3D11ShaderResourceView*>(id)->Release();
}

}  // namespace

const BackendOps& Ops() {
    static const BackendOps ops = {
        &CaptureDevice, &AbandonCapture, &InitRenderer, &NewFrame, &EnsureTarget, &RenderDrawData,
        &InvalidateDeviceObjects, &OnResizeRelease, &OnResizeRecreate,
        &CreateTextureFromImageFile, &DestroyTexture, "DX11"};
    return ops;
}

}  // namespace ui::overlay_backend::dx11
