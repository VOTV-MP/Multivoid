// ui/overlay_seams.cpp -- the engine seams the overlay draws and resizes through. Install() AOB-resolves
// five engine-private functions and MinHooks them: the three per-RHI present choke points (the draw
// seams, FD3D11Viewport::PresentChecked, FD3D12Viewport::PresentInternal and FVulkanViewport::Present)
// and the two per-DX-RHI viewport Resize functions (the render-target release bracket). The DX present
// detours hand each frame to imgui_overlay::OverlayFrame; the resize detours bracket the engine's own
// resize with the backend's release and recreate. No DXGI vtable entry is patched, so RivaTuner has
// nothing of ours to unlink and OBS's game capture sees our pixels (docs/ui.md). The Vulkan seam does not
// run a frame itself: it brings the overlay up on its first call and swaps the loader's present and
// destroy slots, and the frame runs inside the swapped vkQueuePresentKHR (overlay_backend_vk_frame.cpp),
// the only place that has the image index and the semaphore the present waits on.

#include "overlay_seams.h"

#include "ui/overlay_backend.h"
#include "overlay_backend_internal.h"
#include "overlay_backend_vk.h"

#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/scan_ledger.h"  // the draw seam and the resize bracket, each found or missed
#include "ue_wrap/core/sig_scan.h"     // FindPattern -- the engine seam probes
#include "ue_wrap/core/sdk_profile.h"  // the DX seam signatures
#include "ue_wrap/engine/vulkan_rhi.h" // the Vulkan seam's resolvers and viewport read

#include <windows.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace ui::overlay_seams {
namespace {

// The three draw seams, in the order the table of Install lists them. The exe holds all three RHIs, so every
// seam resolves and arms on every run; only the seam of the RHI the game renders with ever fires.
enum DrawSeam : int { kDrawDx11, kDrawDx12, kDrawVulkan, kDrawSeams };
struct DrawSeamState {
    const char* name;
    bool        resolved;
    bool        armed;
};
DrawSeamState g_drawSeams[kDrawSeams] = {{"FD3D11Viewport::PresentChecked", false, false},
                                         {"FD3D12Viewport::PresentInternal", false, false},
                                         {"FVulkanViewport::Present", false, false}};
std::atomic<bool> g_drawFired[kDrawSeams];
std::atomic<bool> g_worldJudged{false};

void NoteFired(DrawSeam seam) {
    if (!g_drawFired[seam].load(std::memory_order_relaxed)) g_drawFired[seam].store(true, std::memory_order_relaxed);
}

// Read `viewport + off`, SEH-guarded: a drifted offset must not fault the render thread.
IDXGISwapChain* RawViewportSwapChain(void* viewport, size_t off) {
    if (!viewport) return nullptr;
    __try {
        return *reinterpret_cast<IDXGISwapChain**>(reinterpret_cast<uint8_t*>(viewport) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The guard covers the QueryInterface, not only the load: QI is a vtable dispatch through the very
// pointer this distrusts. A recook that moves the swapchain field leaves the AOB matching and a
// non-null value at the old offset, so the dispatch is where a drifted offset faults.
bool QiIsSwapChain(IDXGISwapChain* sc) {
    __try {
        IDXGISwapChain* probe = nullptr;
        if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain),
                                      reinterpret_cast<void**>(&probe))) || !probe)
            return false;
        probe->Release();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The swapchain, re-read every call and QI-validated on change: a fullscreen transition replaces
// the object, so a cached pointer would draw into an abandoned swapchain; and QueryInterface is a
// COM call on a ~120 Hz path proving an offset, which drifts between builds, not between frames.
// Fails closed: an unprovable pointer yields null, complained about once per pointer.
IDXGISwapChain* ValidatedSwapChain(void* viewport, size_t off, const char* which,
                                   IDXGISwapChain*& lastOk) {
    IDXGISwapChain* sc = RawViewportSwapChain(viewport, off);
    if (!sc) return nullptr;
    if (sc == lastOk) return sc;
    if (!QiIsSwapChain(sc)) {
        static const void* sComplainedAbout = nullptr;
        if (sComplainedAbout != sc) {
            sComplainedAbout = sc;
            UE_LOGE("imgui_overlay: %s -- the pointer at viewport+0x%zX (%p) does not "
                    "QueryInterface as IDXGISwapChain. The offset has drifted for this "
                    "build; the overlay draws NOTHING rather than through an unidentified "
                    "pointer -- re-derive per docs/versioning.md.",
                    which, off, static_cast<void*>(sc));
            ue_wrap::log::Flush();
        }
        return nullptr;
    }
    lastOk = sc;
    return sc;
}

// The draw seam. An inline patch on IDXGISwapChain::Present is the byte region RivaTuner restores
// (the overlay died after a frame) and sits below where OBS copies the backbuffer;
// FD3D11Viewport::PresentChecked and FD3D12Viewport::PresentInternal are the engine's own
// once-per-frame callers of that Present, upstream of every external hook, found by AOB (the price
// every core hook here pays; docs/versioning.md covers a recook). The draw is unconditional
// (PresentChecked's non-presenting exits would be a site list to mirror; a wasted draw is
// harmless). The return is forwarded as a register-width integer: the DX12 seam tail-jumps into
// Present, whose HRESULT return disagrees with UE's bool declaration, and forwarding RAX byte-exact
// needs no verdict.
using EnginePresentFn = uintptr_t(__fastcall*)(void* viewport, int32_t syncInterval);
EnginePresentFn g_d3d11PresentTrampoline = nullptr;
EnginePresentFn g_d3d12PresentTrampoline = nullptr;
// One proven-pointer cache per RHI, shared by its present seam and its resize bracket: they read
// the same field of the same viewport.
IDXGISwapChain* g_lastOkSc11 = nullptr;
IDXGISwapChain* g_lastOkSc12 = nullptr;

// The window a swapchain presents to, read off its descriptor. SEH-guarded like the other reads of
// engine-owned pointers; null when the descriptor carries no window.
HWND ReadOutputWindow(IDXGISwapChain* sc) {
    __try {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(sc->GetDesc(&desc))) return nullptr;
        return desc.OutputWindow;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The window is the bring-up's alone: it is read only while the overlay is not yet ready, as the
// bring-up always did, and the target carries none afterwards.
HWND BringUpWindow(IDXGISwapChain* sc) {
    return imgui_overlay::ImguiReady() ? nullptr : ReadOutputWindow(sc);
}

uintptr_t __fastcall D3D11PresentCheckedDetour(void* viewport, int32_t syncInterval) {
    NoteFired(kDrawDx11);
    if (IDXGISwapChain* sc = ValidatedSwapChain(viewport, ue_wrap::profile::kD3D11Viewport_SwapChain,
                                                "FD3D11Viewport::PresentChecked",
                                                g_lastOkSc11))
    {
        if (overlay_backend::SelectRhi(overlay_backend::Rhi::Dx11))
            imgui_overlay::OverlayFrame({overlay_backend::Rhi::Dx11, BringUpWindow(sc), sc, nullptr});
    }
    return g_d3d11PresentTrampoline(viewport, syncInterval);
}

uintptr_t __fastcall D3D12PresentInternalDetour(void* viewport, int32_t syncInterval) {
    NoteFired(kDrawDx12);
    if (IDXGISwapChain* sc = ValidatedSwapChain(viewport, ue_wrap::profile::kD3D12Viewport_SwapChain,
                                                "FD3D12Viewport::PresentInternal",
                                                g_lastOkSc12))
    {
        if (overlay_backend::SelectRhi(overlay_backend::Rhi::Dx12))
            imgui_overlay::OverlayFrame({overlay_backend::Rhi::Dx12, BringUpWindow(sc), sc, nullptr});
    }
    return g_d3d12PresentTrampoline(viewport, syncInterval);
}

// The Vulkan seam, FVulkanViewport::Present: the RHI oracle, the bring-up, and the hand-over of the viewport
// to the swapped vkQueuePresentKHR. The engine's Present calls the queue present from inside, on this same
// thread, so a thread-local set around the original is how the swapped present learns which viewport it
// serves. The first call brings the overlay up once, in the order verify -> ImGui init -> draw selftest ->
// ready -> swap the two loader slots LAST, so no present reaches a half-built overlay.
using VulkanPresentFn = bool(__fastcall*)(void* viewport, void* ctx, void* cmd, void* queue,
                                          void* presentQueue, bool lockToVsync);
VulkanPresentFn g_vulkanPresentTrampoline = nullptr;
bool            g_vulkanBringUpTried = false;  // the engine's Present thread is the only writer

void VulkanBringUp(void* viewport, void* presentQueue) {
    ue_wrap::vulkan_rhi::ViewportState st;
    if (!ue_wrap::vulkan_rhi::ReadViewport(viewport, st)) {
        UE_LOGE("[VK-OVERLAY] bring-up refused: the viewport %p could not be read (a fault or an implausible "
                "value) -- overlay down for this run, presents untouched",
                viewport);
        ue_wrap::log::Flush();
        return;
    }
    // The slots are found here and not at Init: the byte walk costs a scan of the image, which only a
    // Vulkan run should pay.
    if (!overlay_backend::vk::ResolveSlots()) {
        UE_LOGE("[VK-OVERLAY] bring-up refused: a loader slot was not found -- overlay down for this run, "
                "presents untouched");
        ue_wrap::log::Flush();
        return;
    }
    // The bring-up's own queue submits (the draw selftest, the first texture upload) run here, on the RHI
    // thread and before the engine's present body. They are as safe as the per-frame submit: the engine's own
    // submits to this queue run on this thread, and the rare ones from another thread were measured never to
    // overlap a call on the present queue.
    overlay_backend::VulkanFrame vf{viewport, nullptr, presentQueue, 0, nullptr};
    const overlay_backend::FrameTarget target{overlay_backend::Rhi::Vulkan, reinterpret_cast<HWND>(st.hwnd),
                                              nullptr, &vf};
    if (!imgui_overlay::BringUpFromSeam(target)) return;
    if (!overlay_backend::vk::SwapSlots()) {
        imgui_overlay::OverlayDown();
        ue_wrap::log::Flush();
    }
}

bool __fastcall VulkanViewportPresentDetour(void* viewport, void* ctx, void* cmd, void* queue,
                                            void* presentQueue, bool lockToVsync) {
    NoteFired(kDrawVulkan);
    if (!overlay_backend::SelectRhi(overlay_backend::Rhi::Vulkan))
        return g_vulkanPresentTrampoline(viewport, ctx, cmd, queue, presentQueue, lockToVsync);
    if (!g_vulkanBringUpTried) {
        g_vulkanBringUpTried = true;
        VulkanBringUp(viewport, presentQueue);
    }
    overlay_backend::vk::SetViewport(viewport);
    const bool result = g_vulkanPresentTrampoline(viewport, ctx, cmd, queue, presentQueue, lockToVsync);
    overlay_backend::vk::SetViewport(nullptr);
    return result;
}

// The resize bracket, on the engine's own seam. IDXGISwapChain::ResizeBuffers fails unless every
// back-buffer reference is released first, and UE turns that failure into a Fatal; our render
// target view is such a reference, and a release bracket on the DXGI function itself is unlinked
// by RivaTuner (measured: the second resize with it armed killed the game). FD3D11Viewport::Resize
// and FD3D12Viewport::ResizeInternal are the engine's private callers, which nothing else patches.
// The swapchain is re-read after the original, never cached: a fullscreen transition can replace
// it. Two signatures: the DX12 seam takes only `this` and reads the extent from a member.
using D3D11ResizeFn = void(__fastcall*)(void* viewport, uint32_t sizeX, uint32_t sizeY,
                                        uint8_t bFullscreen, int32_t pixelFormat);
using D3D12ResizeInternalFn = void(__fastcall*)(void* viewport);
D3D11ResizeFn         g_d3d11ResizeTrampoline = nullptr;
D3D12ResizeInternalFn g_d3d12ResizeTrampoline = nullptr;

// The half both seams share: re-derive the render target after the engine resized. The call to the
// original stays with the caller (the two take different arguments), and `sizeNote` is
// pre-formatted because only one seam has any size to report.
void EngineResizeBracket(const char* which, size_t scOff, overlay_backend::Rhi rhi,
                         IDXGISwapChain*& lastOk,
                         void* viewport, const char* sizeNote) {
    IDXGISwapChain* sc = ValidatedSwapChain(viewport, scOff, which, lastOk);
    if (sc && imgui_overlay::ImguiReady()) {
        overlay_backend::OnResizeRecreate({rhi, nullptr, sc, nullptr});
        // Resizes are rare, so one line each is not spam, and it is the one line that says the
        // bracket ran, with or without RivaTuner in the process.
        UE_LOGI("imgui_overlay: %s%s -- render target rebuilt on %s", which, sizeNote,
                overlay_backend::Kind() ? overlay_backend::Kind() : "?");
        ue_wrap::log::Flush();
    }
    // A null `sc` needs no line: ValidatedSwapChain complained once about the pointer that caused
    // it.
}

void __fastcall D3D11ResizeDetour(void* viewport, uint32_t sizeX, uint32_t sizeY,
                                  uint8_t bFullscreen, int32_t pixelFormat) {
    // The back buffers are about to be recreated.
    overlay_backend::OnResizeRelease(overlay_backend::Rhi::Dx11);
    g_d3d11ResizeTrampoline(viewport, sizeX, sizeY, bFullscreen, pixelFormat);
    char note[96];
    _snprintf_s(note, sizeof(note), _TRUNCATE, " (%ux%u fullscreen=%u fmt=%d)", sizeX, sizeY,
                bFullscreen, pixelFormat);
    EngineResizeBracket("FD3D11Viewport::Resize", ue_wrap::profile::kD3D11Viewport_SwapChain,
                        overlay_backend::Rhi::Dx11, g_lastOkSc11, viewport, note);
}

void __fastcall D3D12ResizeDetour(void* viewport) {
    overlay_backend::OnResizeRelease(overlay_backend::Rhi::Dx12);
    g_d3d12ResizeTrampoline(viewport);
    // No size in the line: ResizeInternal takes only `this`, so four numbers here would be register
    // residue.
    EngineResizeBracket("FD3D12Viewport::ResizeInternal",
                        ue_wrap::profile::kD3D12Viewport_SwapChain, overlay_backend::Rhi::Dx12,
                        g_lastOkSc12, viewport, "");
}

}  // namespace

// The Vulkan seam's entry: the Present function and the pixel-format table the viewport read needs, both
// recorded in the scan ledger by their resolvers. Either missing leaves the seam out (0).
uintptr_t ResolveVulkanSeam() {
    const uintptr_t present = ue_wrap::vulkan_rhi::ResolvePresent();
    if (!present) return 0;
    if (!ue_wrap::vulkan_rhi::ResolvePixelFormatTable()) {
        UE_LOGW("imgui_overlay: FVulkanViewport::Present resolved but the GPixelFormats table did not, so the "
                "back buffer's format cannot be read -- the Vulkan seam is ABSENT this run.");
        return 0;
    }
    return present;
}

bool Install(int& armed, int& total) {
    // Every RHI's seams are installed unconditionally: which one the game presents with is unknown
    // at Init, and the shipped exe holds all three RHIs, so all of them resolve on every run; the one
    // that fires names the RHI, and NoteWorldUp reports a run where none did. A stale
    // signature is not fatal: the overlay keeps drawing and says loudly that resizes are
    // unprotected. The Vulkan seam has no signature string of its own: its resolver is the ue_wrap one.
    struct Seam { const char* name; const char* sig; void* detour; void** tramp; int draw; };
    const Seam seams[] = {
        {"FD3D11Viewport::PresentChecked", ue_wrap::profile::kSigD3D11ViewportPresentChecked,
         reinterpret_cast<void*>(&D3D11PresentCheckedDetour),
         reinterpret_cast<void**>(&g_d3d11PresentTrampoline), kDrawDx11},
        {"FD3D12Viewport::PresentInternal", ue_wrap::profile::kSigD3D12ViewportPresentInternal,
         reinterpret_cast<void*>(&D3D12PresentInternalDetour),
         reinterpret_cast<void**>(&g_d3d12PresentTrampoline), kDrawDx12},
        {"FVulkanViewport::Present", nullptr, reinterpret_cast<void*>(&VulkanViewportPresentDetour),
         reinterpret_cast<void**>(&g_vulkanPresentTrampoline), kDrawVulkan},
        {"FD3D11Viewport::Resize", ue_wrap::profile::kSigD3D11ViewportResize,
         reinterpret_cast<void*>(&D3D11ResizeDetour),
         reinterpret_cast<void**>(&g_d3d11ResizeTrampoline), -1},
        {"FD3D12Viewport::ResizeInternal", ue_wrap::profile::kSigD3D12ViewportResizeInternal,
         reinterpret_cast<void*>(&D3D12ResizeDetour),
         reinterpret_cast<void**>(&g_d3d12ResizeTrampoline), -1},
    };
    total = static_cast<int>(sizeof(seams) / sizeof(seams[0]));
    int drawArmed = 0;
    // The draw row is first decided here by what resolved, and again at the world's first milestone by
    // what fired; the resize row is decided once the loop has tried all five.
    bool drawFound = false, resizeFound = false;
    for (const Seam& sm : seams) {
        const uintptr_t at = sm.sig ? ue_wrap::FindPattern(sm.sig) : ResolveVulkanSeam();
        if (at) (sm.draw >= 0 ? drawFound : resizeFound) = true;
        if (sm.draw >= 0) g_drawSeams[sm.draw].resolved = at != 0;
        if (!at) {
            // A seam whose signature drifted; zero draw seams is checked after the loop.
            UE_LOGW("imgui_overlay: %s signature NOT found -- that seam is ABSENT this "
                    "run.", sm.name);
            continue;
        }
        if (ue_wrap::hook::Install(reinterpret_cast<void*>(at), sm.detour, sm.tramp)) {
            ++armed;
            if (sm.draw >= 0) {
                ++drawArmed;
                g_drawSeams[sm.draw].armed = true;
            }
            UE_LOGI("imgui_overlay: %s %s armed @%p (image+0x%llX)", sm.name,
                    sm.draw >= 0 ? "DRAW seam" : "resize bracket",
                    reinterpret_cast<void*>(at),
                    static_cast<unsigned long long>(
                        at - reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr))));
        } else {
            UE_LOGE("imgui_overlay: %s resolved but MinHook Install FAILED", sm.name);
        }
    }
    ue_wrap::scan_ledger::Record(ue_wrap::scan_ledger::Scan::OverlayDraw, drawFound);
    ue_wrap::scan_ledger::Record(ue_wrap::scan_ledger::Scan::OverlayResize, resizeFound);
    // Fail closed on the draw seam only: with none there is no overlay at all, so this returns
    // false. The window messages are routed either way, and the input swallow in
    // HandleWindowMessage sits behind g_imguiReady, so no key is taken for a UI that cannot
    // appear. A missing resize bracket is a degradation, reported above.
    if (drawArmed == 0) {
        UE_LOGE("imgui_overlay: NO DRAW SEAM. None of FD3D11Viewport::PresentChecked, "
                "FD3D12Viewport::PresentInternal and FVulkanViewport::Present resolved, so the overlay "
                "cannot draw and is DISABLED for this run. This is what a game recook looks like from "
                "here -- re-derive the signatures per docs/versioning.md.");
        ue_wrap::log::Flush();
        return false;
    }
    return true;
}

void NoteWorldUp() {
    if (g_worldJudged.exchange(true)) return;
    bool anyArmed = false;
    for (int i = 0; i < kDrawSeams; ++i) {
        if (g_drawFired[i].load(std::memory_order_relaxed)) return;
        anyArmed = anyArmed || g_drawSeams[i].armed;
    }
    if (!anyArmed) return;  // Install said so already, and the overlay was never set up
    char states[320];
    int at = 0;
    for (int i = 0; i < kDrawSeams && at < static_cast<int>(sizeof states); ++i)
        at += std::snprintf(states + at, sizeof states - static_cast<size_t>(at), "%s%s resolved=%s armed=%s fired=no",
                            i ? "; " : "", g_drawSeams[i].name, g_drawSeams[i].resolved ? "yes" : "no",
                            g_drawSeams[i].armed ? "yes" : "no");
    ue_wrap::scan_ledger::Record(ue_wrap::scan_ledger::Scan::OverlayDraw, false);
    UE_LOGE("imgui_overlay: NO DRAW SEAM. The world is up and no present seam has fired, so the overlay "
            "cannot draw this run (%s). The seam of the RHI the game renders with has drifted -- re-derive "
            "the signatures per docs/versioning.md.",
            states);
    ue_wrap::log::Flush();
}

}  // namespace ui::overlay_seams
