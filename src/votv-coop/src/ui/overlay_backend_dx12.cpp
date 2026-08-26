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
constexpr UINT kTextureSlots     = 256;   // SRV heap: index 0 reserved (see g_tex), 1..N allocatable
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

// SRV slots. ONE POOL, shared by our preview textures and by ImGui's own
// atlas textures -- the hard-coded "slot 0 = the ImGui font" reservation is
// gone. It could only ever describe ONE ImGui texture, and 1.92's atlas keeps up
// to two alive at a time (ImFontAtlasTextureAdd creates the new one before the
// old is destroyed, imgui_draw.cpp:4085-4113), so a privileged single slot is
// not a thing the new backend can be given. ImGui now asks for descriptors
// through SrvAlloc/SrvFree below, out of the same free list as everything else.
//
// Index 0 is deliberately NOT allocatable: it stays the "no slot" sentinel that
// QueuePendingRelease uses for staging buffers, AND it is the descriptor handed
// out if the pool is ever exhausted -- see SrvAlloc. Allocatable range is
// 1..kTextureSlots either way, so no capacity is lost.
struct TexSlot {
    // Owner is explicit rather than inferred from (res, gpuPtr). It used to be:
    // both clear = free, res set = live, res clear + gpuPtr set = awaiting a
    // fence. That encoding has no room for a fourth state, and ImGui-owned slots
    // are exactly that -- occupied, but with a resource WE must never release.
    enum class Owner : uint8_t { Free, Ours, Imgui, Pending };
    ID3D12Resource* res = nullptr;   // OUR resources only; ImGui owns its own
    UINT64 gpuPtr = 0;
    Owner  owner = Owner::Free;
};
TexSlot g_tex[kTextureSlots + 1];

// ImGui's DX12 texture uploads run on their own queue, as they did before: the
// legacy ImGui_ImplDX12_Init we are replacing called CreateCommandQueue itself
// (imgui_impl_dx12.cpp:973) and set commandQueueOwned. Handing it g_queue -- the
// game's PRESENTING queue -- would newly serialize its uploads behind the game's
// frame work, and ImGui_ImplDX12_UpdateTexture ends in
// WaitForSingleObject(..., INFINITE) (imgui_impl_dx12.cpp:~565), which is not a
// wait this codebase permits anywhere else (kFenceWaitMs bounds every one of
// ours). Keeping it on a separate queue preserves today's behaviour exactly;
// removing the unbounded wait needs ImDrawData::Textures = nullptr and our own
// servicing, which belongs with the flag flip, not here.
ID3D12CommandQueue* g_imguiTexQueue = nullptr;

// Deferred release: a resource may still be read by a submitted list, so it is
// freed only once the fence passes its recorded value. Slots recycle then too.
struct Pending {
    ID3D12Resource* res = nullptr;
    UINT slot = 0;          // 0 = nothing to recycle (staging buffers)
    UINT64 fenceValue = 0;
};

// THIS QUEUE HAS NO FIXED SIZE, ON PURPOSE. It used to be Pending[64], justified
// by a comment ("never seen: 64 entries vs a ~10-preview UI") that priced the
// wrong producer: a texture SLOT is bounded by kTextureSlots, but every
// CreateTexture also queues its STAGING buffer with slot = 0, and staging
// entries are not bounded by slot count at all -- N creates inside one fence
// window are N entries whatever N is. So the same set was bounded twice, in two
// different units, in two places, and the overflow path Release()d immediately
// while the GPU could still be reading: a live latent use-after-free whose own
// comment named the risk. [[lesson-one-capacity-expressed-in-three-places-will-disagree]]
//
// Picking a bigger number would restate the bug. Removing the bound deletes it:
// the only remaining bound is how many resources one fence window can produce,
// which is exactly the quantity the queue is FOR. Growth is self-limiting --
// ProcessPendingReleases compacts every frame, and a fence that stops advancing
// stops production too (the WaitFence in CreateTexture takes the device-removed
// path and returns nullptr before anything is queued).
std::vector<Pending> g_pending;   // live entries only; compacted as fences pass

void QueuePendingRelease(ID3D12Resource* res, UINT slot, UINT64 fenceValue) {
    if (!res && !slot) return;
    if (slot && slot <= kTextureSlots) g_tex[slot].owner = TexSlot::Owner::Pending;
    g_pending.push_back(Pending{ res, slot, fenceValue });
}

// Descriptor arithmetic for a slot. One place, so our textures and ImGui's
// cannot disagree about where a slot lives.
UINT SrvStride() {
    return g_device ? g_device->GetDescriptorHandleIncrementSize(
                          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) : 0;
}
D3D12_CPU_DESCRIPTOR_HANDLE SlotCpu(UINT slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(SrvStride()) * slot;
    return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE SlotGpu(UINT slot) {
    D3D12_GPU_DESCRIPTOR_HANDLE h = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(SrvStride()) * slot;
    return h;
}

// 0 = pool exhausted. A slot is reusable only once ProcessPendingReleases has
// cleared it, never while it is Pending -- recycling a descriptor under an
// in-flight draw is correctness audit C-1 (2026-07-26).
UINT AllocSlot() {
    for (UINT i = 1; i <= kTextureSlots; ++i)
        if (g_tex[i].owner == TexSlot::Owner::Free) return i;
    return 0;
}

void ProcessPendingReleases() {
    if (!g_fence || g_pending.empty()) return;  // the common per-frame case
    const UINT64 done = g_fence->GetCompletedValue();
    size_t keep = 0;
    for (size_t i = 0; i < g_pending.size(); ++i) {
        const Pending p = g_pending[i];   // by value: the compaction writes behind i
        if (p.fenceValue > done) { g_pending[keep++] = p; continue; }
        if (p.res) p.res->Release();
        // Only clear a slot still marked Pending -- never stomp a live entry that
        // was reallocated in the meantime.
        if (p.slot && p.slot <= kTextureSlots &&
            g_tex[p.slot].owner == TexSlot::Owner::Pending)
            g_tex[p.slot] = TexSlot{};
    }
    g_pending.resize(keep);
}

// ImGui's descriptor allocator. Called from ImGui_ImplDX12_UpdateTexture on
// WantCreate, and its Free counterpart on destroy.
void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
              D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
    const UINT slot = AllocSlot();
    if (!slot) {
        // ImGui's callback signature has no failure channel, and it WILL write an
        // SRV to whatever we return. Index 0 exists for exactly this: a real
        // descriptor inside our own heap, so an exhausted pool aliases a texture
        // (visibly wrong) instead of scribbling outside the heap (a GPU fault).
        UE_LOGE("imgui_overlay: dx12 SRV pool exhausted (%u slots) -- ImGui texture "
                "aliases the reserve descriptor; some UI image will draw wrong",
                kTextureSlots);
        *outCpu = SlotCpu(0);
        *outGpu = SlotGpu(0);
        return;
    }
    g_tex[slot].owner  = TexSlot::Owner::Imgui;
    g_tex[slot].res    = nullptr;      // ImGui owns the resource, we own the slot
    g_tex[slot].gpuPtr = SlotGpu(slot).ptr;
    *outCpu = SlotCpu(slot);
    *outGpu = SlotGpu(slot);
}

void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE,
             D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
    for (UINT i = 1; i <= kTextureSlots; ++i)
        if (g_tex[i].owner == TexSlot::Owner::Imgui && g_tex[i].gpuPtr == gpu.ptr) {
            // Fence-deferred like ours: the descriptor may still be referenced by
            // a submitted draw. res = nullptr because the resource is ImGui's.
            QueuePendingRelease(nullptr, i, g_lastSignaled);
            return;
        }
}

// ONE place that builds the InitInfo, because InitRenderer and the desc-change
// re-init must agree exactly. Replaces the legacy 6-argument
// ImGui_ImplDX12_Init, whose whole contract was "here is ONE descriptor for the
// font" -- it cannot describe 1.92's atlas, which keeps up to two textures alive
// across a repack, and it is also what STRIPPED
// ImGuiBackendFlags_RendererHasTextures (imgui_impl_dx12.cpp:987), i.e. what
// forced DX12 to a different drawable repertoire than DX11.
bool InitImguiBackend() {
    // Its own queue, exactly as the legacy path had: that overload called
    // CreateCommandQueue itself (imgui_impl_dx12.cpp:973). Handing it g_queue
    // would newly serialize ImGui's texture uploads behind the game's frame work
    // AND put ImGui_ImplDX12_UpdateTexture's WaitForSingleObject(..., INFINITE)
    // on the presenting queue. Keeping the queue separate preserves today's
    // behaviour byte for byte; the unbounded wait itself is only reachable once
    // the dynamic atlas is on, and removing it needs ImDrawData::Textures =
    // nullptr plus our own bounded servicing -- that belongs with the flag flip.
    if (!g_imguiTexQueue) {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        qd.NodeMask = 1;
        if (FAILED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_imguiTexQueue))) ||
            !g_imguiTexQueue) {
            UE_LOGE("imgui_overlay: dx12 could not create the ImGui texture-upload queue");
            return false;
        }
    }
    ImGui_ImplDX12_InitInfo info{};
    info.Device               = g_device;
    info.CommandQueue         = g_imguiTexQueue;
    info.NumFramesInFlight    = static_cast<int>(g_bufferCount);
    info.RTVFormat            = g_rtvFormat;
    info.SrvDescriptorHeap    = g_srvHeap;
    info.SrvDescriptorAllocFn = &SrvAlloc;
    info.SrvDescriptorFreeFn  = &SrvFree;
    if (!ImGui_ImplDX12_Init(&info)) return false;
    // THE FLAG STAYS ON (2026-07-30, the flip), and this is where the
    // transitional clear used to be. ImGui_ImplDX12_Init(InitInfo) leaves
    // ImGuiBackendFlags_RendererHasTextures set -- the whole point of the struct
    // is that the backend CAN service multiple textures -- and now that it does,
    // that is the regime we want. The clear is deleted, not conditioned (RULE 2);
    // it only ever existed to keep DX12 and DX11 in the SAME regime for one
    // build, because one binary with two drawable repertoires chosen by the
    // player's GPU API means two peers agree about who collided (the fold is a
    // build constant) while disagreeing about what the names look like.
    //
    // The DX12-specific risk this leaves is upload cost, not correctness: the
    // atlas now grows during play instead of once at boot, and every growth goes
    // through ImGui_ImplDX12_UpdateTexture's INFINITE fence wait. That is what
    // the probe below measures, and it is why TexMaxWidth/Height is pinned at
    // 2048 in ui/fonts.cpp.
    return true;
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
    // Released AFTER ImGui_ImplDX12_Shutdown has run (Shutdown() calls it before
    // us), so the backend is done submitting on it. The legacy path owned this
    // queue itself and released it in its own Shutdown; now we own it, so we must.
    if (g_imguiTexQueue) { g_imguiTexQueue->Release(); g_imguiTexQueue = nullptr; }
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
    if (!InitImguiBackend()) {
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
    if (!InitImguiBackend()) {
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

// THE UPLOAD PROBE, and it measures a path that ALREADY RUNS -- it is not new
// work introduced by the flip.
//
// ImGui assigns draw_data->Textures unconditionally (imgui.cpp, in Render) and
// ImGui_ImplDX12_RenderDrawData services that list ungated by the capability
// flag, so upstream's upload path has been executing at boot and twice per
// rescale in every build since the 1.92 upgrade, and had never been timed. What
// the flip changes is FREQUENCY: the atlas now grows and repacks during play.
//
// Two properties make it worth a permanent probe rather than a one-off:
//
//   THE WAIT IS UNBOUNDED. ImGui_ImplDX12_UpdateTexture ends in a
//   WaitForSingleObject(.., INFINITE) on the render thread. DX11's
//   UpdateSubresource has no fence and no wait, so this is a DX12-only exposure.
//
//   THE BOX ACCUMULATES. ImGui::Render() runs unconditionally while THIS
//   function early-outs on six conditions above (facade disabled, swapchain
//   rebind, queue re-confirmation in flight). During such a window glyphs keep
//   baking and the texture's dirty UpdateRect keeps growing, and the first
//   serviced frame pays for the whole window at once. So the probe logs the
//   accumulated box, not only the elapsed time.
//
// It then NULLS draw_data->Textures so the backend does not repeat the work --
// the servicing has happened, and this is exactly the seam our own servicing
// would replace if the numbers ever demand one. Nulling here is DX12-only and
// cannot starve DX11: that backend reads the same field from its own frame.
void ServiceTexturesTimed(ImDrawData* dd) {
    if (!dd || !dd->Textures) return;
    static double s_lastLog = 0.0;
    static double s_worstMs = 0.0;
    LARGE_INTEGER freq{}, a{}, b{};
    ::QueryPerformanceFrequency(&freq);
    int serviced = 0, boxW = 0, boxH = 0;
    double totalMs = 0.0;
    for (ImTextureData* tex : *dd->Textures) {
        if (!tex || tex->Status == ImTextureStatus_OK) continue;
        const ImTextureRect& r = tex->UpdateRect;
        ::QueryPerformanceCounter(&a);
        ImGui_ImplDX12_UpdateTexture(tex);
        ::QueryPerformanceCounter(&b);
        const double ms = freq.QuadPart
            ? (double(b.QuadPart - a.QuadPart) * 1000.0 / double(freq.QuadPart)) : 0.0;
        totalMs += ms;
        ++serviced;
        if (r.w > boxW) boxW = r.w;
        if (r.h > boxH) boxH = r.h;
    }
    dd->Textures = nullptr;   // serviced above; do not let the backend redo it
    if (serviced == 0) return;
    // Log the first upload of a run, then only a new worst case, then a heartbeat
    // -- enough to price the path without a per-frame line.
    const double now = ImGui::GetTime();
    const bool worse = totalMs > s_worstMs + 0.5;
    if (worse) s_worstMs = totalMs;
    if (worse || s_lastLog == 0.0 || now - s_lastLog > 60.0) {
        s_lastLog = now;
        UE_LOGI("imgui_overlay: dx12 texture upload -- %d texture(s), %.2f ms total, dirty "
                "box up to %dx%d (INFINITE fence wait; worst so far %.2f ms)",
                serviced, totalMs, boxW, boxH, s_worstMs);
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
    ImDrawData* dd = ImGui::GetDrawData();
    ServiceTexturesTimed(dd);
    ImGui_ImplDX12_RenderDrawData(dd, g_list);

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
        UE_LOGI("imgui_overlay: dx12 first frame rendered (%d vertices, %d draw list(s), "
                "backbuffer %u)", dd ? dd->TotalVtxCount : -1, dd ? dd->CmdLists.Size : -1, idx);
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

    // A slot whose release is still PENDING must not be handed out again: that
    // would overwrite a shader-visible SRV descriptor under an in-flight draw and
    // then get wiped by ProcessPendingReleases (correctness audit C-1,
    // 2026-07-26). AllocSlot answers Free only, and it is now the SAME free list
    // ImGui draws from.
    const UINT slot = AllocSlot();
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

    const D3D12_CPU_DESCRIPTOR_HANDLE cpu = SlotCpu(slot);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu = SlotGpu(slot);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(tex, &srvd, cpu);

    g_tex[slot].res    = tex;
    g_tex[slot].gpuPtr = gpu.ptr;
    g_tex[slot].owner  = TexSlot::Owner::Ours;
    if (outW) *outW = static_cast<int>(w);
    if (outH) *outH = static_cast<int>(h);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(gpu.ptr));
}

void DestroyTexture(void* id) {
    if (!id) return;
    const UINT64 ptr = static_cast<UINT64>(reinterpret_cast<uintptr_t>(id));
    for (UINT i = 1; i <= kTextureSlots; ++i)
        if (g_tex[i].owner == TexSlot::Owner::Ours && g_tex[i].gpuPtr == ptr) {
            // The GPU may still be reading it this frame: release + recycle the
            // slot only once the current work has passed the fence.
            QueuePendingRelease(g_tex[i].res, i, g_lastSignaled);
            g_tex[i].res = nullptr;  // the slot is freed by ProcessPendingReleases
            return;
        }
}


}  // namespace ui::overlay_backend::dx12
