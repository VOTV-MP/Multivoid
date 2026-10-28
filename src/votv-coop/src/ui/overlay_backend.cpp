// ui/overlay_backend.cpp -- the RHI dispatcher behind ui/overlay_backend.h.
//
// Owns exactly one concept: WHICH per-RHI implementation is live. The engine seam that fired
// (the D3D11, the D3D12 or the Vulkan present detour) names it through SelectRhi before the frame runs;
// every entry below forwards to that RHI's ops table. DX12 additionally has to capture its
// presenting command queue first (a few frames -- see overlay_backend_dx12_capture.cpp).

#include "ui/overlay_backend.h"

#include "overlay_backend_internal.h"
#include "overlay_backend_vk.h"
#include "ue_wrap/core/log.h"

#include "imgui.h"

#include <windows.h>
#include <wincodec.h>  // WIC: the skins-browser preview decode (PNG/BMP -> BGRA)

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace ui::overlay_backend {
namespace {

const BackendOps* OpsFor(Rhi rhi) {
    switch (rhi) {
        case Rhi::Dx11: return &dx11::Ops();
        case Rhi::Dx12: return &dx12::Ops();
        case Rhi::Vulkan: return &vk::Ops();
        default:        return nullptr;
    }
}

const BackendOps* g_ops = nullptr;  // render-thread-owned (all writers run in the Present detour)
bool g_brought = false;             // an InitRenderer succeeded: the RHI is latched for the run

}  // namespace

bool SelectRhi(Rhi rhi) {
    const BackendOps* want = OpsFor(rhi);
    if (g_brought && want != g_ops) {
        // A backend that was never captured or initialised must not run a frame, so the seam of
        // the other RHI is refused; a second viewport is the only way one could fire.
        static bool warned = false;
        if (!warned) {
            warned = true;
            UE_LOGW("imgui_overlay: the %s present seam fired after the overlay came up on %s -- "
                    "that seam's frames are ignored for the rest of the run",
                    want ? want->name : "unknown-RHI", g_ops->name);
        }
        return false;
    }
    g_ops = want;
    return want != nullptr;
}

const char* Kind() { return g_ops ? g_ops->name : nullptr; }

bool CaptureDevice(const FrameTarget& t) {
    return g_ops && g_ops->capture(t);  // true only once the presenting queue is CONFIRMED
}

void AbandonCapture() {
    if (g_ops) g_ops->abandon();
}

bool InitRenderer(const FrameTarget& t) {
    if (!g_ops || !g_ops->init(t)) return false;
    g_brought = true;
    return true;
}

void NewFrame() {
    if (g_ops) g_ops->newFrame();
}

void InvalidateDeviceObjects() {
    if (g_ops) g_ops->invalidate();
}

void EnsureTarget(const FrameTarget& t) {
    if (g_ops) g_ops->ensureTarget(t);
}

void RenderDrawData(const FrameTarget& t) {
    if (g_ops) g_ops->render(t);
}

// The resize pair is the DX resize bracket's mechanism: a half without one leaves it null. The
// backend is the one of the seam that fired, not the last selected one.
void OnResizeRelease(Rhi rhi) {
    const BackendOps* ops = OpsFor(rhi);
    if (ops && ops->resizeRelease) ops->resizeRelease();
}

void OnResizeRecreate(const FrameTarget& t) {
    const BackendOps* ops = OpsFor(t.rhi);
    if (ops && ops->resizeRecreate) ops->resizeRecreate(t);
}

void* CreateTextureFromImageFile(const wchar_t* path, int* outW, int* outH) {
    return g_ops ? g_ops->textureFromFile(path, outW, outH) : nullptr;
}

ImTextureRef TextureRef(void* handle) {
    if (g_ops && g_ops->textureRef) return g_ops->textureRef(handle);
    return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(handle)));
}

void DestroyTexture(void* handle) {
    if (g_ops) g_ops->destroyTexture(handle);
}

void InstallCreationProbe() { dx12_capture::InstallCreationProbe(); }

void RestoreEngineSlots() { vk::RestoreSlots(); }

// RULE 2: there is deliberately no Shutdown() here, and nothing needs one. InitRenderer's
// failure paths call ReleaseRendererState(); at process exit the OS reclaims what that released;
// and the one thing a dying process actually needs -- stopping new detour entries -- is
// hook::Shutdown's blanket disable.

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
