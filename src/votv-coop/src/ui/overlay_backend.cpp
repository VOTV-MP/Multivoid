// ui/overlay_backend.cpp -- the RHI dispatcher behind ui/overlay_backend.h.
//
// Owns exactly one concept: WHICH per-RHI implementation is live. The first
// present's CaptureDevice picks it by what the game's swapchain actually is:
// a DX11 device comes straight off the swapchain, while DX12 additionally has
// to capture its presenting command queue first (a few frames -- see
// overlay_backend_dx12_capture.cpp). Design of record:
// research/findings/tooling/votv-imgui-dx12-overlay-DESIGN-2026-07-26.md.

#include "ui/overlay_backend.h"

#include "overlay_backend_internal.h"

#include <windows.h>
#include <wincodec.h>  // WIC: the skins-browser preview decode (PNG/BMP -> BGRA)

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace ui::overlay_backend {
namespace {

enum class Active { None, Dx11, Dx12 };
Active g_active = Active::None;  // render-thread-owned (all writers run in the Present detour)

}  // namespace

const char* Kind() {
    if (g_active == Active::Dx11 && dx11::Live()) return "DX11";
    if (g_active == Active::Dx12 && dx12::Live()) return "DX12";
    return nullptr;
}

bool CaptureDevice(IDXGISwapChain* sc) {
    if (dx11::CaptureDevice(sc)) {
        g_active = Active::Dx11;
        return true;
    }
    if (dx12::CaptureDevice(sc)) {  // true only once the presenting queue is CONFIRMED
        g_active = Active::Dx12;
        return true;
    }
    return false;
}

void AbandonCapture() {
    if (g_active == Active::Dx11) dx11::AbandonCapture();
    else if (g_active == Active::Dx12) dx12::AbandonCapture();
    g_active = Active::None;
}

bool InitRenderer(IDXGISwapChain* sc) {
    if (g_active == Active::Dx11) return dx11::InitRenderer(sc);
    if (g_active == Active::Dx12) return dx12::InitRenderer(sc);
    return false;
}

void NewFrame() {
    if (g_active == Active::Dx11) dx11::NewFrame();
    else if (g_active == Active::Dx12) dx12::NewFrame();
}

void InvalidateDeviceObjects() {
    if (g_active == Active::Dx11) dx11::InvalidateDeviceObjects();
    else if (g_active == Active::Dx12) dx12::InvalidateDeviceObjects();
}

void EnsureTarget(IDXGISwapChain* sc) {
    if (g_active == Active::Dx11) dx11::EnsureTarget(sc);
    else if (g_active == Active::Dx12) dx12::EnsureTarget(sc);
}

void RenderDrawData(IDXGISwapChain* sc) {
    if (g_active == Active::Dx11) dx11::RenderDrawData(sc);
    else if (g_active == Active::Dx12) dx12::RenderDrawData(sc);
}

void OnResizeRelease() {
    if (g_active == Active::Dx11) dx11::OnResizeRelease();
    else if (g_active == Active::Dx12) dx12::OnResizeRelease();
}

void OnResizeRecreate(IDXGISwapChain* sc) {
    if (g_active == Active::Dx11) dx11::OnResizeRecreate(sc);
    else if (g_active == Active::Dx12) dx12::OnResizeRecreate(sc);
}

void* CreateTextureFromImageFile(const wchar_t* path, int* outW, int* outH) {
    if (g_active == Active::Dx11) return dx11::CreateTextureFromImageFile(path, outW, outH);
    if (g_active == Active::Dx12) return dx12::CreateTextureFromImageFile(path, outW, outH);
    return nullptr;
}

void DestroyTexture(void* id) {
    if (g_active == Active::Dx11) dx11::DestroyTexture(id);
    else if (g_active == Active::Dx12) dx12::DestroyTexture(id);
}

void InstallCreationProbe() { dx12_capture::InstallCreationProbe(); }

// RULE 2, 2026-08-26: a `Shutdown()` used to live here. `[V]` It was reachable ONLY from
// ui::imgui_overlay::Shutdown(), which had zero callers tree-wide for its entire life and
// was deleted in 42af8cc0 -- so this ran exactly never. No InitRenderer failure path used
// it either; those call ReleaseRendererState(). At process exit the OS reclaims what it
// released, and the one thing a dying process actually needs -- stop new detour entries --
// is hook::Shutdown's blanket disable. See docs/UE4SS_ARC.md section 4c.

namespace detail {

bool DecodeImageFileBgra(const wchar_t* path, std::vector<uint8_t>& outBgra,
                         unsigned& outW, unsigned& outH) {
    if (!path) return false;
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
        return false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT w = 0, h = 0;
    bool ok = false;
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
        outBgra.assign(static_cast<size_t>(w) * h * 4, 0);
        if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(outBgra.size()),
                                    outBgra.data())))
            break;
        ok = true;
    } while (false);
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    fac->Release();
    if (ok) { outW = w; outH = h; }
    return ok;
}

}  // namespace detail

}  // namespace ui::overlay_backend
