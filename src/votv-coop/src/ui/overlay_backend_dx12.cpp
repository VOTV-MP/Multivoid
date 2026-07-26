// ui/overlay_backend_dx12.cpp -- the D3D12 RENDERER half of
// ui/overlay_backend.h. The presenting-queue capture (and the stage-1
// measurement behind it) lives in overlay_backend_dx12_capture.cpp; this TU
// takes the confirmed queue and draws.
//
// Frame sync is the vendored example_win32_directx12 FrameContext shape (a
// per-context fence value; WAIT before the allocator Reset, Signal after
// Execute) -- lifted rather than re-derived. Every wait is BOUNDED and checks
// GetDeviceRemovedReason on timeout: a TDR must never hang the render thread
// or shutdown.
//
// Design of record:
// research/findings/tooling/votv-imgui-dx12-overlay-DESIGN-2026-07-26.md

#include "ui/overlay_backend.h"

#include "overlay_backend_internal.h"
#include "ue_wrap/core/log.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"

namespace ui::overlay_backend::dx12 {

namespace {

constexpr UINT kMaxBackBuffers   = 8;
constexpr UINT kTextureSlots     = 256;   // SRV heap: slot 0 = ImGui font, 1..N = UI textures
constexpr DWORD kFenceWaitMs     = 2000;  // the house bound (see the design doc)

ID3D12CommandQueue* g_queue = nullptr;     // the CONFIRMED presenting queue (AddRef'd)
IDXGISwapChain*     g_boundSc = nullptr;   // the swapchain our state belongs to (identity only)
IDXGISwapChain3*    g_sc3 = nullptr;

ID3D12DescriptorHeap* g_rtvHeap = nullptr;
ID3D12DescriptorHeap* g_srvHeap = nullptr;
D3D12_CPU_DESCRIPTOR_HANDLE g_rtvHandles[kMaxBackBuffers] = {};
ID3D12Resource* g_backBuffers[kMaxBackBuffers] = {};
UINT g_bufferCount = 0;
DXGI_FORMAT g_rtvFormat = DXGI_FORMAT_UNKNOWN;

struct FrameContext {
    ID3D12CommandAllocator* allocator = nullptr;
    UINT64 fenceValue = 0;
};
FrameContext g_frames[kMaxBackBuffers];
UINT g_frameIndex = 0;

ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12CommandAllocator*    g_uploadAlloc = nullptr;
ID3D12GraphicsCommandList* g_uploadList  = nullptr;
UINT64 g_uploadFence = 0;

ID3D12Fence* g_fence = nullptr;
HANDLE g_fenceEvent = nullptr;
UINT64 g_lastSignaled = 0;

bool g_live = false;        // InitRenderer committed
bool g_disabled = false;    // device-removed: stop drawing for the rest of the run
bool g_firstFrameLogged = false;
ID3D12Device* g_device = nullptr;  // borrowed from dx12_capture (it owns the ref)

// SRV slots + the resources behind them (slot 0 is the font, owned by ImGui).
struct TexSlot {
    ID3D12Resource* res = nullptr;
    UINT64 gpuPtr = 0;
};
TexSlot g_tex[kTextureSlots + 1];

// Deferred release: a resource may still be read by a submitted list, so it is
// freed only once the fence passes its recorded value. Slots recycle then too.
struct Pending {
    ID3D12Resource* res = nullptr;
    UINT slot = 0;          // 0 = nothing to recycle (staging buffers)
    UINT64 fenceValue = 0;
};
Pending g_pending[64];
int g_pendingCount = 0;   // non-empty entries in g_pending (skips the per-frame walk)

void QueuePendingRelease(ID3D12Resource* res, UINT slot, UINT64 fenceValue) {
    if (!res && !slot) return;
    for (auto& p : g_pending)
        if (!p.res && !p.slot) {
            p.res = res; p.slot = slot; p.fenceValue = fenceValue;
            ++g_pendingCount;
            return;
        }
    // Table full (never seen: 64 entries vs a ~10-preview UI). Release now --
    // and say so, because a silent immediate release is a use-after-free risk.
    UE_LOGW("imgui_overlay: dx12 deferred-release table full -- releasing %p immediately",
            static_cast<void*>(res));
    if (res) res->Release();
    if (slot && slot <= kTextureSlots && !g_tex[slot].res) g_tex[slot] = TexSlot{};
}

void ProcessPendingReleases() {
    if (!g_fence || g_pendingCount == 0) return;  // the common per-frame case
    const UINT64 done = g_fence->GetCompletedValue();
    for (auto& p : g_pending) {
        if ((!p.res && !p.slot) || p.fenceValue > done) continue;
        if (p.res) p.res->Release();
        // Only clear a slot that is still the one we deferred (its res is null
        // and its gpuPtr is the pending marker) -- never stomp a live entry.
        if (p.slot && p.slot <= kTextureSlots && !g_tex[p.slot].res)
            g_tex[p.slot] = TexSlot{};
        p = Pending{};
        --g_pendingCount;
    }
}

void NoteDeviceRemoved(const char* where) {
    if (g_disabled) return;
    g_disabled = true;
    const HRESULT reason = g_device ? g_device->GetDeviceRemovedReason() : E_FAIL;
    UE_LOGE("imgui_overlay: RHI = DX12: GPU wait timed out in %s (removed-reason=0x%08lX) -- "
            "overlay rendering disabled for this run; the game keeps presenting.", where, reason);
    ue_wrap::log::Flush();
}

// Bounded fence wait. false = timed out (device-removed path taken).
bool WaitFence(UINT64 value, const char* where) {
    if (!g_fence || value == 0) return true;
    if (g_fence->GetCompletedValue() >= value) return true;
    if (g_fenceEvent && SUCCEEDED(g_fence->SetEventOnCompletion(value, g_fenceEvent))) {
        if (::WaitForSingleObject(g_fenceEvent, kFenceWaitMs) == WAIT_OBJECT_0) return true;
        NoteDeviceRemoved(where);
        return false;
    }
    // No event (or it could not be armed): SPIN with the same bound rather than
    // claiming success -- a false success resets allocators under live GPU work
    // (audit I-4). ~1 ms granularity; this path is not the steady state.
    for (DWORD waited = 0; waited < kFenceWaitMs; waited += 1) {
        if (g_fence->GetCompletedValue() >= value) return true;
        ::Sleep(1);
    }
    NoteDeviceRemoved(where);
    return false;
}

bool WaitGpuIdle(const char* where) { return WaitFence(g_lastSignaled, where); }

void ReleaseSwapchainDerived() {
    for (UINT i = 0; i < kMaxBackBuffers; ++i)
        if (g_backBuffers[i]) { g_backBuffers[i]->Release(); g_backBuffers[i] = nullptr; }
    if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; }
    if (g_sc3) { g_sc3->Release(); g_sc3 = nullptr; }
}

// (Re)derive everything that depends on THIS swapchain's desc: the sc3
// interface, the RTV heap + per-buffer RTVs, the buffer count and the RTV
// format. false = the swapchain could not be read; the caller leaves the state
// null and the next present retries.
bool CreateSwapchainDerived(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (!g_device || FAILED(sc->GetDesc(&desc))) return false;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&g_sc3))) || !g_sc3) return false;
    if (desc.BufferCount > kMaxBackBuffers)
        UE_LOGW("imgui_overlay: dx12: swapchain has %u buffers (cap %u) -- frames beyond the cap "
                "will not draw", desc.BufferCount, kMaxBackBuffers);
    g_bufferCount = desc.BufferCount > kMaxBackBuffers ? kMaxBackBuffers : desc.BufferCount;
    g_rtvFormat = desc.BufferDesc.Format;

    D3D12_DESCRIPTOR_HEAP_DESC rd{};
    rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rd.NumDescriptors = g_bufferCount;
    rd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&g_rtvHeap))) || !g_rtvHeap) {
        ReleaseSwapchainDerived();
        return false;
    }
    const UINT rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_bufferCount; ++i) {
        g_rtvHandles[i] = h;
        h.ptr += rtvSize;
        if (SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]))) && g_backBuffers[i])
            g_device->CreateRenderTargetView(g_backBuffers[i], nullptr, g_rtvHandles[i]);
    }
    g_boundSc = sc;
    return true;
}

}  // namespace

namespace {

// Free every renderer-owned object (NOT the capture-owned device/queue). Used
// by Shutdown and by every InitRenderer failure path -- imgui_overlay's
// bring-up contract is "on ANY failure it releases whatever it acquired this
// call", and the DX12 half was leaking a heap+RTVs+allocators+fence per retry
// (correctness audit I-1, 2026-07-26).
void ReleaseRendererState() {
    ReleaseSwapchainDerived();
    for (auto& f : g_frames)
        if (f.allocator) { f.allocator->Release(); f.allocator = nullptr; }
    if (g_list) { g_list->Release(); g_list = nullptr; }
    if (g_uploadList) { g_uploadList->Release(); g_uploadList = nullptr; }
    if (g_uploadAlloc) { g_uploadAlloc->Release(); g_uploadAlloc = nullptr; }
    if (g_srvHeap) { g_srvHeap->Release(); g_srvHeap = nullptr; }
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_fenceEvent) { ::CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
}

// The backend's PSO bakes the RTV format and the frames-in-flight count at
// Init, so a real format/count change needs a backend re-init (the ImGui
// context and the Win32 backend survive). Shared by the resize bracket and the
// swapchain-recreation branch (audit I-2: only one of them had it).
void ReinitBackendIfDescChanged(UINT oldCount, DXGI_FORMAT oldFormat) {
    if (g_bufferCount == oldCount && g_rtvFormat == oldFormat) return;
    UE_LOGI("imgui_overlay: dx12: swapchain desc changed (buffers %u->%u, format %d->%d) -- "
            "re-initializing the renderer backend", oldCount, g_bufferCount,
            static_cast<int>(oldFormat), static_cast<int>(g_rtvFormat));
    WaitGpuIdle("desc-change");
    ImGui_ImplDX12_Shutdown();
    if (!ImGui_ImplDX12_Init(g_device, static_cast<int>(g_bufferCount), g_rtvFormat, g_srvHeap,
                             g_srvHeap->GetCPUDescriptorHandleForHeapStart(),
                             g_srvHeap->GetGPUDescriptorHandleForHeapStart())) {
        UE_LOGE("imgui_overlay: dx12 re-init after a desc change FAILED -- overlay disabled");
        g_disabled = true;
    }
}

}  // namespace

bool Live() { return g_live; }  // "DX12 is the active backend", even if drawing is disabled

bool CaptureDevice(IDXGISwapChain* sc) {
    if (g_live) return true;
    ID3D12CommandQueue* confirmed = dx12_capture::TryConfirmQueue(sc);
    g_device = dx12_capture::Device();
    if (!confirmed) return false;
    g_queue = confirmed;
    g_queue->AddRef();
    return true;
}

void AbandonCapture() {
    if (g_queue) { g_queue->Release(); g_queue = nullptr; }
}

bool InitRenderer(IDXGISwapChain* sc) {
    if (!g_device || !g_queue) return false;
    if (!CreateSwapchainDerived(sc)) {
        UE_LOGE("imgui_overlay: dx12 bring-up failed -- swapchain-derived state (RTVs)");
        ReleaseRendererState();
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC sd{};
    sd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sd.NumDescriptors = kTextureSlots + 1;  // slot 0 = font
    sd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&g_srvHeap))) || !g_srvHeap) {
        UE_LOGE("imgui_overlay: dx12 bring-up failed -- SRV heap");
        ReleaseRendererState();
        return false;
    }
    // ALL kMaxBackBuffers allocators, not just the current g_bufferCount: a
    // later swapchain recreation can come back with MORE buffers (up to the
    // cap) and RenderDrawData indexes g_frames by the CURRENT count. Sizing to
    // the boot-time count left the extra slots null -> a null deref on the
    // render thread (perf audit CRIT-1, 2026-07-26). Allocators are cheap.
    for (UINT i = 0; i < kMaxBackBuffers; ++i)
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&g_frames[i].allocator)))) {
            UE_LOGE("imgui_overlay: dx12 bring-up failed -- command allocator %u", i);
            ReleaseRendererState();
            return false;
        }
    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&g_uploadAlloc))) ||
        FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_uploadAlloc,
                                           nullptr, IID_PPV_ARGS(&g_uploadList))) ||
        FAILED(g_uploadList->Close())) {
        UE_LOGE("imgui_overlay: dx12 bring-up failed -- upload list");
        ReleaseRendererState();
        return false;
    }
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           g_frames[0].allocator, nullptr,
                                           IID_PPV_ARGS(&g_list))) ||
        FAILED(g_list->Close())) {
        UE_LOGE("imgui_overlay: dx12 bring-up failed -- command list");
        ReleaseRendererState();
        return false;
    }
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
        UE_LOGE("imgui_overlay: dx12 bring-up failed -- fence");
        ReleaseRendererState();
        return false;
    }
    g_fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent) {
        // Without the event every WaitFence would fake success and we would
        // Reset allocators under live GPU work (audit I-4).
        UE_LOGE("imgui_overlay: dx12 bring-up failed -- fence event");
        ReleaseRendererState();
        return false;
    }
    if (!ImGui_ImplDX12_Init(g_device, static_cast<int>(g_bufferCount), g_rtvFormat, g_srvHeap,
                             g_srvHeap->GetCPUDescriptorHandleForHeapStart(),
                             g_srvHeap->GetGPUDescriptorHandleForHeapStart())) {
        UE_LOGE("imgui_overlay: ImGui_ImplDX12_Init failed");
        ReleaseRendererState();
        return false;
    }
    g_live = true;
    UE_LOGI("imgui_overlay: dx12 renderer up -- %u buffers, rtvFormat=%d, queue=%p",
            g_bufferCount, static_cast<int>(g_rtvFormat), static_cast<void*>(g_queue));
    ue_wrap::log::Flush();
    return true;
}

void NewFrame() {
    if (!g_disabled) ImGui_ImplDX12_NewFrame();
}

void InvalidateDeviceObjects() {
    // The font texture may still be referenced by in-flight lists (DX11 hid
    // this behind driver refcounting; D3D12 does not).
    WaitGpuIdle("InvalidateDeviceObjects");
    ImGui_ImplDX12_InvalidateDeviceObjects();
}

void EnsureTarget(IDXGISwapChain* sc) {
    if (!g_live) return;
    ProcessPendingReleases();  // RenderDrawData may not run for many frames (audit MINOR-7)
    if (g_disabled) return;
    if (sc != g_boundSc) {
        // A swapchain RECREATION never passes through our ResizeBuffers hook.
        // Two things must happen, and the first one is NOT optional: the new
        // chain may be presented by a DIFFERENT queue, and submitting our list
        // on the wrong queue is a cross-queue race on the backbuffer -- not "a
        // stale frame" as an earlier comment claimed (correctness audit I-2).
        // So: stop drawing, RE-ARM the capture seeded with the queue we know,
        // and only draw again once it is re-confirmed.
        UE_LOGI("imgui_overlay: dx12: swapchain changed (%p -> %p) -- rebuilding targets and "
                "re-confirming the presenting queue",
                static_cast<void*>(g_boundSc), static_cast<void*>(sc));
        WaitGpuIdle("swapchain-recreate");
        ReleaseSwapchainDerived();
        const UINT oldCount = g_bufferCount;
        const DXGI_FORMAT oldFormat = g_rtvFormat;
        if (!CreateSwapchainDerived(sc)) return;
        ReinitBackendIfDescChanged(oldCount, oldFormat);
        if (g_queue) { g_queue->Release(); g_queue = nullptr; }
        dx12_capture::Rearm();  // seeds the previously confirmed queue as candidate
        return;
    }
    if (!g_rtvHeap) CreateSwapchainDerived(sc);
    if (!g_queue) {  // re-arm in flight: draw again only once re-confirmed
        if (ID3D12CommandQueue* q = dx12_capture::TryConfirmQueue(sc)) {
            g_queue = q;
            g_queue->AddRef();
            UE_LOGI("imgui_overlay: dx12: presenting queue re-confirmed after the swapchain "
                    "change -- drawing resumes");
        }
    }
}

void RenderDrawData(IDXGISwapChain* sc) {
    if (!g_live || g_disabled || !g_rtvHeap || !g_sc3 || !g_queue || sc != g_boundSc) return;
    FrameContext& fc = g_frames[g_frameIndex % g_bufferCount];
    ++g_frameIndex;
    if (!fc.allocator) return;  // belt to CRIT-1's braces (all slots are created up front)
    if (!WaitFence(fc.fenceValue, "frame-context")) return;
    fc.fenceValue = 0;
    if (FAILED(fc.allocator->Reset())) return;
    if (FAILED(g_list->Reset(fc.allocator, nullptr))) return;

    const UINT idx = g_sc3->GetCurrentBackBufferIndex();
    // Past this point the list is RECORDING: every early return must Close it,
    // or the next frame resets an allocator whose list is still open (invalid
    // D3D12 usage -- audit LOW-12).
    if (idx >= g_bufferCount || !g_backBuffers[idx]) { g_list->Close(); return; }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = g_backBuffers[idx];
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_list->ResourceBarrier(1, &barrier);

    g_list->OMSetRenderTargets(1, &g_rtvHandles[idx], FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {g_srvHeap};
    g_list->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_list);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_list->ResourceBarrier(1, &barrier);
    if (FAILED(g_list->Close())) return;

    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    const UINT64 value = ++g_lastSignaled;
    if (SUCCEEDED(g_queue->Signal(g_fence, value))) fc.fenceValue = value;
    ProcessPendingReleases();

    if (!g_firstFrameLogged) {
        g_firstFrameLogged = true;
        const ImDrawData* dd = ImGui::GetDrawData();
        UE_LOGI("imgui_overlay: dx12 first frame rendered (%d vertices, %d draw list(s), "
                "backbuffer %u)", dd ? dd->TotalVtxCount : -1, dd ? dd->CmdListsCount : -1, idx);
        ue_wrap::log::Flush();
    }
}

void OnResizeRelease() {
    if (!g_live) return;
    WaitGpuIdle("resize-release");  // the GPU may still read the buffers we are about to drop
    ReleaseSwapchainDerived();
}

void OnResizeRecreate(IDXGISwapChain* sc) {
    if (!g_live || g_disabled) return;
    const UINT oldCount = g_bufferCount;
    const DXGI_FORMAT oldFormat = g_rtvFormat;
    if (!CreateSwapchainDerived(sc)) return;
    ReinitBackendIfDescChanged(oldCount, oldFormat);
}

void* CreateTextureFromImageFile(const wchar_t* path, int* outW, int* outH) {
    if (!g_live || g_disabled || !path) return nullptr;
    std::vector<uint8_t> px;
    unsigned w = 0, h = 0;
    if (!detail::DecodeImageFileBgra(path, px, w, h)) return nullptr;

    UINT slot = 0;
    // A slot whose release is still PENDING keeps its gpuPtr: handing it out
    // again would overwrite a shader-visible SRV descriptor under an in-flight
    // draw and then get wiped by ProcessPendingReleases (correctness audit
    // C-1, 2026-07-26). Free == both fields clear.
    for (UINT i = 1; i <= kTextureSlots; ++i)
        if (!g_tex[i].res && !g_tex[i].gpuPtr) { slot = i; break; }
    if (!slot) {
        UE_LOGW("imgui_overlay: dx12 preview slots exhausted (%u) -- this image renders blank",
                kTextureSlots);
        return nullptr;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ID3D12Resource* tex = nullptr;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&tex))) || !tex)
        return nullptr;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT numRows = 0; UINT64 rowBytes = 0, totalBytes = 0;
    g_device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &numRows, &rowBytes, &totalBytes);
    D3D12_HEAP_PROPERTIES uhp{};
    uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ud{};
    ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ud.Width = totalBytes; ud.Height = 1; ud.DepthOrArraySize = 1; ud.MipLevels = 1;
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.SampleDesc.Count = 1;
    ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* upload = nullptr;
    if (FAILED(g_device->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &ud,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&upload))) || !upload) {
        tex->Release();
        return nullptr;
    }
    uint8_t* mapped = nullptr;
    D3D12_RANGE noRead{0, 0};
    if (FAILED(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped))) || !mapped) {
        upload->Release(); tex->Release();
        return nullptr;
    }
    for (UINT y = 0; y < h; ++y)
        std::memcpy(mapped + static_cast<size_t>(fp.Footprint.RowPitch) * y,
                    px.data() + static_cast<size_t>(w) * 4 * y, static_cast<size_t>(w) * 4);
    upload->Unmap(0, nullptr);

    // Queue-ordered upload: this list goes to the SAME queue BEFORE the frame's
    // draw list, so no CPU wait is needed for the copy to become visible.
    if (!WaitFence(g_uploadFence, "texture-upload")) {
        upload->Release(); tex->Release();
        return nullptr;
    }
    if (FAILED(g_uploadAlloc->Reset()) || FAILED(g_uploadList->Reset(g_uploadAlloc, nullptr))) {
        upload->Release(); tex->Release();
        return nullptr;
    }
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    g_uploadList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER tb{};
    tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    tb.Transition.pResource = tex;
    tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    tb.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g_uploadList->ResourceBarrier(1, &tb);
    g_uploadList->Close();
    ID3D12CommandList* ul[] = {g_uploadList};
    g_queue->ExecuteCommandLists(1, ul);
    g_uploadFence = ++g_lastSignaled;
    g_queue->Signal(g_fence, g_uploadFence);
    QueuePendingRelease(upload, 0, g_uploadFence);  // staging dies once the copy is done

    const UINT srvSize =
        g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(srvSize) * slot;
    gpu.ptr += static_cast<UINT64>(srvSize) * slot;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(tex, &srvd, cpu);

    g_tex[slot].res = tex;
    g_tex[slot].gpuPtr = gpu.ptr;
    if (outW) *outW = static_cast<int>(w);
    if (outH) *outH = static_cast<int>(h);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(gpu.ptr));
}

void DestroyTexture(void* id) {
    if (!id) return;
    const UINT64 ptr = static_cast<UINT64>(reinterpret_cast<uintptr_t>(id));
    for (UINT i = 1; i <= kTextureSlots; ++i)
        if (g_tex[i].res && g_tex[i].gpuPtr == ptr) {
            // The GPU may still be reading it this frame: release + recycle the
            // slot only once the current work has passed the fence.
            QueuePendingRelease(g_tex[i].res, i, g_lastSignaled);
            g_tex[i].res = nullptr;  // the slot is freed by ProcessPendingReleases
            return;
        }
}

void Shutdown(bool rendererWasLive) {
    dx12_capture::Shutdown();  // disarm the capture hooks + drop its device ref
    if (!g_live && !g_device) return;
    WaitGpuIdle("shutdown");
    for (auto& p : g_pending) {
        if (p.res) p.res->Release();
        p = Pending{};
    }
    g_pendingCount = 0;
    for (UINT i = 1; i <= kTextureSlots; ++i)
        if (g_tex[i].res) { g_tex[i].res->Release(); g_tex[i] = TexSlot{}; }
    if (rendererWasLive && g_live) ImGui_ImplDX12_Shutdown();
    g_live = false;
    ReleaseRendererState();
    if (g_queue) { g_queue->Release(); g_queue = nullptr; }
    g_device = nullptr;  // owned by dx12_capture
}

}  // namespace ui::overlay_backend::dx12
