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

#include "ue_wrap/core/log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>  // WIC: the skins-browser preview decode (PNG/BMP -> BGRA)

#include <atomic>
#include <cstdint>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

namespace ui::overlay_backend {
namespace {

ID3D11Device*           g_device  = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
ID3D11RenderTargetView* g_rtv     = nullptr;
bool                    g_live    = false;   // InitRenderer committed
std::atomic<bool> g_dx12Logged{false};  // logged the DX12-unsupported notice once

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

const char* Kind() { return g_live ? "DX11" : nullptr; }

bool CaptureDevice(IDXGISwapChain* sc) {
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev))) || !dev) {
        if (dev) dev->Release();
        if (!g_dx12Logged.exchange(true)) {
            UE_LOGW("imgui_overlay: swapchain is NOT DX11 (likely DX12) -- overlay "
                    "rendering not yet implemented for this RHI; menu will not draw.");
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
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device)  { g_device->Release();  g_device = nullptr; }
}

bool InitRenderer(IDXGISwapChain* sc) {
    if (!ImGui_ImplDX11_Init(g_device, g_context)) {
        UE_LOGE("imgui_overlay: ImGui_ImplDX11_Init failed");
        return false;
    }
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
    // thread that owns g_device). WIC decode (PNG/BMP/JPG) -> 32bpp BGRA ->
    // immutable D3D11 texture + SRV. The returned SRV is the ImTextureID; the
    // caller caches it (this is a per-file one-shot, not a per-frame path).
    if (!g_device || !path) return nullptr;
    static bool s_comTried = false;
    if (!s_comTried) {
        s_comTried = true;
        // S_FALSE (already init) and RPC_E_CHANGED_MODE (STA already active on
        // this thread) both leave COM usable for CoCreateInstance below.
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    IWICImagingFactory* fac = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&fac))) || !fac)
        return nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT w = 0, h = 0;
    do {
        if (FAILED(fac->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &dec)) || !dec)
            break;
        if (FAILED(dec->GetFrame(0, &frame)) || !frame) break;
        if (FAILED(fac->CreateFormatConverter(&conv)) || !conv) break;
        if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom)))
            break;
        if (FAILED(conv->GetSize(&w, &h)) || !w || !h || w > 4096 || h > 4096) break;
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data())))
            break;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{px.data(), w * 4u, 0};
        if (FAILED(g_device->CreateTexture2D(&td, &sd, &tex)) || !tex) break;
        g_device->CreateShaderResourceView(tex, nullptr, &srv);
    } while (false);
    if (tex) tex->Release();  // the SRV holds its own reference
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    fac->Release();
    if (srv) {
        if (outW) *outW = static_cast<int>(w);
        if (outH) *outH = static_cast<int>(h);
    }
    return srv;
}

void Shutdown(bool rendererWasLive) {
    if (rendererWasLive) ImGui_ImplDX11_Shutdown();
    g_live = false;
    ReleaseRTV();
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device)  { g_device->Release();  g_device = nullptr; }
}

}  // namespace ui::overlay_backend
