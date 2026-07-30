// ui/overlay_backend_dx11.cpp -- the D3D11 implementation of ui/overlay_backend.h.
//
// Bodies are a literal move out of imgui_overlay.cpp (2026-07-26 extraction;
// design of record research/findings/tooling/votv-imgui-dx12-overlay-DESIGN-
// 2026-07-26.md). One documented deviation: the old BringUpDX11 committed
// g_device/g_context only after Win32+DX11 init both succeeded; the split
// commits them in CaptureDevice and relies on the AbandonCapture unwind arm
// instead -- same net behavior (single-threaded on the render thread; nothing
// reads these globals between the two bring-up steps).

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
bool                    g_live    = false;   // InitRenderer committed

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

}  // namespace

bool Live() { return g_live; }

bool CaptureDevice(IDXGISwapChain* sc) {
    // Silent on a non-DX11 swapchain: the DISPATCHER owns the RHI verdict and
    // the DX12 path's own logging (the old one-shot "NOT DX11" WARN moved
    // there, replaced by the stage-1 detection lines -- RULE 2).
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev))) || !dev) {
        if (dev) dev->Release();
        return false;
    }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    g_device = dev;
    g_context = ctx;
    return true;
}

void AbandonCapture() {
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device)  { g_device->Release();  g_device = nullptr; }
}

bool InitRenderer(IDXGISwapChain* sc) {
    if (!ImGui_ImplDX11_Init(g_device, g_context)) {
        UE_LOGE("imgui_overlay: ImGui_ImplDX11_Init failed");
        return false;
    }
    // TRANSITIONAL, RETIRED BY THE DX12 InitInfo COMMIT. 1.92's DX11 backend
    // declares ImGuiBackendFlags_RendererHasTextures unconditionally
    // (imgui_impl_dx11.cpp:632) while DX12's legacy init STRIPS it
    // (imgui_impl_dx12.cpp:987: "with 1 SRV descriptor we cannot support
    // multiple textures"). Left alone, ONE build would ship TWO drawable
    // repertoires chosen by the player's RHI -- the dynamic atlas draws whatever
    // the face cmaps carry (~8,148 codepoints) while the preload path draws the
    // repertoire table's 2,517 -- so the font selftest's verdict, the nickname
    // fold and every screenshot would mean different things on different
    // machines, and this commit would not be bisectable.
    //
    // Clearing it here holds BOTH RHIs in the legacy regime, which is MEASURED
    // equivalent to 1.91.5: atlas->Locked is set for the whole frame
    // (imgui.cpp:9089-9094), so an out-of-repertoire codepoint is not baked and
    // draws FallbackGlyph exactly as before -- 0 texels diverged from the single
    // upload, 0 glyph UVs moved, TexIsBuilt held (probe arm L, 2026-07-30).
    // Once DX12 runs on ImGui_ImplDX12_InitInfo with our own descriptor
    // allocator, this line goes and the flag stays on for both.
    ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
    g_live = true;
    CreateRTV(sc);
    return true;
}

void NewFrame() { ImGui_ImplDX11_NewFrame(); }

void InvalidateDeviceObjects() { ImGui_ImplDX11_InvalidateDeviceObjects(); }

void EnsureTarget(IDXGISwapChain* sc) {
    if (!g_rtv) CreateRTV(sc);  // recreate after a resize
}

void RenderDrawData(IDXGISwapChain*) {
    if (g_rtv) {
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

void OnResizeRelease() { ReleaseRTV(); }

void OnResizeRecreate(IDXGISwapChain* sc) { CreateRTV(sc); }

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

void Shutdown(bool rendererWasLive) {
    if (rendererWasLive) ImGui_ImplDX11_Shutdown();
    g_live = false;
    ReleaseRTV();
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device)  { g_device->Release();  g_device = nullptr; }
}

}  // namespace ui::overlay_backend::dx11
