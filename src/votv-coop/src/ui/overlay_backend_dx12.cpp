// ui/overlay_backend_dx12.cpp -- the D3D12 half of ui/overlay_backend.h.
//
// STAGE 1 (this commit): capture instrumentation ONLY -- the renderer is
// commit 3 of the design of record (research/findings/tooling/
// votv-imgui-dx12-overlay-DESIGN-2026-07-26.md). Everything here exists to
// MEASURE, on a real -dx12 run, the pillars the renderer will stand on:
//   P1  sc->GetDevice(ID3D12Device) succeeds            (hr logged)
//   P3  the swapchain QIs to IDXGISwapChain3            (hr logged)
//   the presenting-queue capture: which ID3D12CommandQueue instances call
//   ExecuteCommandLists (per-caller: ptr, Desc.Type, thread id, device-match,
//   call count), and how often the last DIRECT same-device ECL before a
//   Present is the SAME queue (the candidate agreement tally that anchors the
//   prod confirmation constants). The creation probe measures whether our
//   boot precedes swapchain creation (pDevice of CreateSwapChain* is the
//   presenting queue on D3D12 -- the zero-ambiguity capture IF we precede).
// OFF-edge: after kTallyPresents presents the summary is flushed and the
// hooks are hook::Disable'd (patch lifted, trampoline slot retained on
// purpose -- see ue_wrap/core/hook.h). Known-positive rule: the tally is
// accepted only on a corpus that COULD disagree -- the summary counts
// non-candidate ECL traffic (copy/compute queues) so a silent instrument is
// distinguishable from a quiet world.

#include "ui/overlay_backend.h"

#include "overlay_backend_internal.h"
#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdint>

namespace ui::overlay_backend::dx12 {
namespace {

// ---- stage-1 state -----------------------------------------------------------

constexpr int kTallyPresents = 600;  // ~10 s at 60 fps; the OFF-edge

ID3D12Device* g_device = nullptr;          // AddRef'd at detection; the device-match anchor
std::atomic<bool> g_detectLogged{false};   // one-shot detection block
std::atomic<bool> g_eclHookInstalled{false};  // published by the worker, read by the render thread
bool g_stage1Done = false;                 // OFF-edge reached (render-thread owned)
int  g_presents = 0;

using EclFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
EclFn g_origEcl = nullptr;
void* g_eclTarget = nullptr;

// CreateSwapChain probes (installed at boot from imgui_overlay::Init).
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*,
                                                      DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                             const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                             IDXGIOutput*, IDXGISwapChain1**);
CreateSwapChainFn        g_origCreateSc     = nullptr;
CreateSwapChainForHwndFn g_origCreateScHwnd = nullptr;
void* g_createScTarget     = nullptr;
void* g_createScHwndTarget = nullptr;

// Per-queue tally. Fixed slots; registration is a one-time CAS per queue.
struct QueueSlot {
    std::atomic<ID3D12CommandQueue*> q{nullptr};
    D3D12_COMMAND_QUEUE_DESC desc{};
    DWORD firstTid = 0;
    bool  deviceMatch = false;
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> presentWins{0};  // was the last DIRECT same-device ECL before a Present
};
constexpr int kMaxQueues = 16;
QueueSlot g_queues[kMaxQueues];
std::atomic<int> g_queueCount{0};
std::atomic<ID3D12CommandQueue*> g_lastDirectSameDev{nullptr};  // reset each Present
std::atomic<uint64_t> g_droppedQueueRegs{0};  // >kMaxQueues distinct queues (never silent)

QueueSlot* FindOrRegister(ID3D12CommandQueue* q) {
    const int n = g_queueCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_queues[i].q.load(std::memory_order_relaxed) == q) return &g_queues[i];
    // First sight of this queue: claim a slot (rare path).
    for (int i = 0; i < kMaxQueues; ++i) {
        ID3D12CommandQueue* expected = nullptr;
        if (g_queues[i].q.load(std::memory_order_relaxed) == q) return &g_queues[i];
        if (g_queues[i].q.compare_exchange_strong(expected, q, std::memory_order_acq_rel)) {
            QueueSlot& s = g_queues[i];
            s.desc = q->GetDesc();
            s.firstTid = ::GetCurrentThreadId();
            ID3D12Device* qdev = nullptr;
            if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&qdev))) && qdev) {
                s.deviceMatch = (qdev == g_device);
                qdev->Release();
            }
            int cnt = g_queueCount.load(std::memory_order_relaxed);
            while (cnt <= i && !g_queueCount.compare_exchange_weak(cnt, i + 1,
                                                                   std::memory_order_release)) {}
            UE_LOGI("imgui_overlay: dx12 stage1: queue %p first seen -- type=%d prio=%d tid=%lu "
                    "device-match=%d",
                    static_cast<void*>(q), static_cast<int>(s.desc.Type),
                    static_cast<int>(s.desc.Priority), s.firstTid, s.deviceMatch ? 1 : 0);
            return &s;
        }
    }
    g_droppedQueueRegs.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void STDMETHODCALLTYPE EclDetour(ID3D12CommandQueue* q, UINT numLists,
                                 ID3D12CommandList* const* lists) {
    if (QueueSlot* s = FindOrRegister(q)) {
        s->calls.fetch_add(1, std::memory_order_relaxed);
        if (s->desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && s->deviceMatch)
            g_lastDirectSameDev.store(q, std::memory_order_relaxed);
    }
    g_origEcl(q, numLists, lists);
}

// ---- creation probes ---------------------------------------------------------

void LogCreationDevice(const char* which, IUnknown* pDevice) {
    ID3D12CommandQueue* q = nullptr;
    HRESULT qi = E_NOINTERFACE;
    if (pDevice) qi = pDevice->QueryInterface(IID_PPV_ARGS(&q));
    UE_LOGI("imgui_overlay: dx12 stage1: %s fired -- pDevice=%p QI(ID3D12CommandQueue) hr=0x%08lX"
            " queue=%p tid=%lu",
            which, static_cast<void*>(pDevice), qi, static_cast<void*>(q),
            ::GetCurrentThreadId());
    if (q) q->Release();
}

HRESULT STDMETHODCALLTYPE CreateScDetour(IDXGIFactory* self, IUnknown* pDevice,
                                         DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    LogCreationDevice("CreateSwapChain", pDevice);
    return g_origCreateSc(self, pDevice, desc, out);
}

HRESULT STDMETHODCALLTYPE CreateScHwndDetour(IDXGIFactory2* self, IUnknown* pDevice, HWND hwnd,
                                             const DXGI_SWAP_CHAIN_DESC1* desc,
                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                             IDXGIOutput* restrict_, IDXGISwapChain1** out) {
    LogCreationDevice("CreateSwapChainForHwnd", pDevice);
    return g_origCreateScHwnd(self, pDevice, hwnd, desc, fs, restrict_, out);
}

// ---- stage-1 pieces ----------------------------------------------------------

// Off the render thread (see LogDetectionOnce): make a throwaway D3D12
// device + DIRECT queue purely to read ID3D12CommandQueue's vtable, then arm
// the capture detour. MinHook freezes threads itself, so installing from this
// worker is safe.
DWORD WINAPI EclHookThread(LPVOID) {
    using CreateDevFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    HMODULE d3d12 = ::LoadLibraryW(L"d3d12.dll");
    CreateDevFn create = d3d12 ? reinterpret_cast<CreateDevFn>(
                                     ::GetProcAddress(d3d12, "D3D12CreateDevice"))
                               : nullptr;
    if (!create) {
        UE_LOGW("imgui_overlay: dx12 stage1: D3D12CreateDevice unavailable -- ECL hook skipped");
        return 0;
    }
    ID3D12Device* dummyDev = nullptr;
    if (FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dummyDev))) || !dummyDev) {
        UE_LOGW("imgui_overlay: dx12 stage1: dummy D3D12 device failed -- ECL hook skipped");
        return 0;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* dummyQ = nullptr;
    if (SUCCEEDED(dummyDev->CreateCommandQueue(&qd, IID_PPV_ARGS(&dummyQ))) && dummyQ) {
        void** vtbl = *reinterpret_cast<void***>(dummyQ);
        g_eclTarget = vtbl[10];  // ID3D12CommandQueue::ExecuteCommandLists
        if (ue_wrap::hook::Install(g_eclTarget, reinterpret_cast<void*>(&EclDetour),
                                   reinterpret_cast<void**>(&g_origEcl))) {
            g_eclHookInstalled = true;  // published after the hook is live
            UE_LOGI("imgui_overlay: dx12 stage1: ExecuteCommandLists hook armed (@%p) -- "
                    "tallying %d presents", g_eclTarget, kTallyPresents);
            ue_wrap::log::Flush();
        } else {
            g_eclTarget = nullptr;
        }
        dummyQ->Release();
    }
    dummyDev->Release();
    return 0;
}

void LogDetectionOnce(IDXGISwapChain* sc) {
    if (g_detectLogged.exchange(true)) return;
    // P1: the D3D12 device off the swapchain.
    ID3D12Device* dev = nullptr;
    const HRESULT hrDev = sc->GetDevice(IID_PPV_ARGS(&dev));
    // P3: the flip-model interface with GetCurrentBackBufferIndex.
    IDXGISwapChain3* sc3 = nullptr;
    const HRESULT hrSc3 = sc->QueryInterface(IID_PPV_ARGS(&sc3));
    DXGI_SWAP_CHAIN_DESC desc{};
    const HRESULT hrDesc = sc->GetDesc(&desc);
    UE_LOGW("imgui_overlay: RHI = DX12 -- render path not built yet (stage-1 capture "
            "instrumentation running); menu will not draw.");
    UE_LOGI("imgui_overlay: dx12 stage1: P1 GetDevice(ID3D12Device) hr=0x%08lX dev=%p | "
            "P3 QI(IDXGISwapChain3) hr=0x%08lX | desc hr=0x%08lX bufferCount=%u format=%d "
            "swapEffect=%d windowed=%d",
            hrDev, static_cast<void*>(dev), hrSc3, hrDesc, desc.BufferCount,
            static_cast<int>(desc.BufferDesc.Format), static_cast<int>(desc.SwapEffect),
            desc.Windowed ? 1 : 0);
    if (sc3) sc3->Release();
    if (dev) {
        g_device = dev;  // keep the AddRef from GetDevice: the device-match anchor
    }
    ue_wrap::log::Flush();  // INFO rides the CRT buffer (log.cpp: flush only on WARN/ERROR);
                            // a gate line nobody can read is not a measurement
    if (!g_device) return;
    // The ECL capture hook needs a throwaway D3D12 device + queue to read the
    // vtable. That work runs on a ONE-SHOT WORKER THREAD, never here: this is
    // the Present detour, i.e. inside the game's DXGI Present call, and
    // creating a device under DXGI's own lock is exactly the shape that
    // deadlocks a render thread. (It was NOT observed to hang on this rig --
    // the 2026-07-26 "log stops dead" symptom turned out to be unflushed INFO
    // lines lost to a force-kill -- but the render thread is the wrong place
    // for device creation regardless.) d3d12.dll stays lazily loaded: a DX11
    // run never touches it.
    ::CloseHandle(::CreateThread(nullptr, 0, &EclHookThread, nullptr, 0, nullptr));
}

void FlushSummaryAndDisarm() {
    const int n = g_queueCount.load(std::memory_order_acquire);
    uint64_t foreignCalls = 0;  // non-(DIRECT+device-match) traffic = the known-positive
    for (int i = 0; i < n; ++i) {
        QueueSlot& s = g_queues[i];
        ID3D12CommandQueue* q = s.q.load(std::memory_order_relaxed);
        if (!q) continue;
        const uint64_t calls = s.calls.load(std::memory_order_relaxed);
        const uint64_t wins = s.presentWins.load(std::memory_order_relaxed);
        const bool candidate = s.desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && s.deviceMatch;
        if (!candidate) foreignCalls += calls;
        UE_LOGI("imgui_overlay: dx12 stage1 SUMMARY: queue %p type=%d tid=%lu device-match=%d "
                "calls=%llu lastBeforePresent=%llu/%d (%.1f%%)",
                static_cast<void*>(q), static_cast<int>(s.desc.Type), s.firstTid,
                s.deviceMatch ? 1 : 0, static_cast<unsigned long long>(calls),
                static_cast<unsigned long long>(wins), g_presents,
                g_presents ? 100.0 * static_cast<double>(wins) / g_presents : 0.0);
    }
    UE_LOGI("imgui_overlay: dx12 stage1 SUMMARY: %d queue(s), %d presents tallied, foreign "
            "(non-candidate) ECL calls=%llu%s, dropped-regs=%llu -- hooks disarming "
            "(disable-only; trampoline slots retained)",
            n, g_presents, static_cast<unsigned long long>(foreignCalls),
            foreignCalls ? "" : " [WARN: corpus could not disagree -- extend the run in-world]",
            static_cast<unsigned long long>(g_droppedQueueRegs.load(std::memory_order_relaxed)));
    ue_wrap::log::Flush();  // the gate reads this summary; never leave it buffered
    if (g_eclHookInstalled.exchange(false)) ue_wrap::hook::Disable(g_eclTarget);
    if (g_createScTarget)     ue_wrap::hook::Disable(g_createScTarget);
    if (g_createScHwndTarget) ue_wrap::hook::Disable(g_createScHwndTarget);
}

}  // namespace

void InstallCreationProbe() {
    // Resolve the factory vtable off a throwaway factory. dxgi.dll is loaded
    // by the game (and by our dummy-DX11 vtable resolver) at this point; go
    // through GetProcAddress so we add no import.
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE dxgi = ::GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) { UE_LOGW("imgui_overlay: creation probe skipped -- dxgi.dll not loaded"); return; }
    CreateFactoryFn createFactory =
        reinterpret_cast<CreateFactoryFn>(::GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (!createFactory) { UE_LOGW("imgui_overlay: creation probe skipped -- no CreateDXGIFactory1"); return; }
    IDXGIFactory1* fac = nullptr;
    if (FAILED(createFactory(IID_PPV_ARGS(&fac))) || !fac) {
        UE_LOGW("imgui_overlay: creation probe skipped -- CreateDXGIFactory1 failed");
        return;
    }
    void** vtbl = *reinterpret_cast<void***>(fac);
    g_createScTarget = vtbl[10];  // IDXGIFactory::CreateSwapChain
    IDXGIFactory2* fac2 = nullptr;
    if (SUCCEEDED(fac->QueryInterface(IID_PPV_ARGS(&fac2))) && fac2) {
        void** vtbl2 = *reinterpret_cast<void***>(fac2);
        g_createScHwndTarget = vtbl2[15];  // IDXGIFactory2::CreateSwapChainForHwnd
        fac2->Release();
    }
    fac->Release();
    if (!ue_wrap::hook::Install(g_createScTarget, reinterpret_cast<void*>(&CreateScDetour),
                                reinterpret_cast<void**>(&g_origCreateSc)))
        g_createScTarget = nullptr;
    if (g_createScHwndTarget &&
        !ue_wrap::hook::Install(g_createScHwndTarget,
                                reinterpret_cast<void*>(&CreateScHwndDetour),
                                reinterpret_cast<void**>(&g_origCreateScHwnd)))
        g_createScHwndTarget = nullptr;
    UE_LOGI("imgui_overlay: swapchain-creation probe armed (CreateSwapChain=%p ForHwnd=%p)",
            g_createScTarget, g_createScHwndTarget);
}

void PendingTick(IDXGISwapChain* sc) {
    if (g_stage1Done) return;
    LogDetectionOnce(sc);
    if (!g_eclHookInstalled.load(std::memory_order_acquire)) return;  // tally starts when armed
    // Present tick: score the last DIRECT same-device ECL seen since the
    // previous Present, then reset the marker for the next frame.
    ID3D12CommandQueue* winner = g_lastDirectSameDev.exchange(nullptr, std::memory_order_relaxed);
    if (winner) {
        const int n = g_queueCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            if (g_queues[i].q.load(std::memory_order_relaxed) == winner) {
                g_queues[i].presentWins.fetch_add(1, std::memory_order_relaxed);
                break;
            }
    }
    if (++g_presents >= kTallyPresents) {
        g_stage1Done = true;
        FlushSummaryAndDisarm();
    }
}

void Stage1Shutdown() {
    if (g_eclHookInstalled.exchange(false)) ue_wrap::hook::Disable(g_eclTarget);
    // The creation probes + the disabled ECL hook are removed wholesale by
    // hook::Shutdown's MH_Uninitialize (the pre-existing teardown class; the
    // accepted-residual note lives in the design doc).
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

}  // namespace ui::overlay_backend::dx12
