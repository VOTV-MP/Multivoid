// ui/overlay_backend_dx12_capture.cpp -- WHICH command queue presents this
// swapchain.
//
// D3D12 exposes no API for that (unlike the device, which comes off the
// swapchain directly), so the presenting queue is captured by hooking
// ID3D12CommandQueue::ExecuteCommandLists and watching which DIRECT
// same-device queue is the last to submit before each Present. A candidate
// must hold that position for a confirmation window before the renderer is
// brought up on it -- a foreign same-device DIRECT writer (another overlay)
// cannot steal the capture, and nothing is drawn until the queue is confirmed.
//
// This TU also carries the stage-1 instrumentation that MEASURED the pillars
// the DX12 backend stands on (a real -dx12 run, 2026-07-26, rig CLIENT_3):
//   P1  sc->GetDevice(ID3D12Device)  hr=0            -> TRUE
//   P3  QI(IDXGISwapChain3)          hr=0            -> TRUE (backbuffer index)
//   swapchain: 3 buffers, FLIP_DISCARD, format 24 (R10G10B10A2_UNORM) -- the
//   renderer takes the RTV format from the desc, never a literal.
//   queues: ONE DIRECT device-matched queue (2999 calls, last-before-Present
//   600/600 = 100%), one COPY queue (5 calls) -- the known-positive proving
//   the instrument can see foreign traffic; no ambiguity, no HALT.
//   the creation probe (CreateSwapChain/ForHwnd, whose pDevice IS the
//   presenting queue on D3D12) armed but NEVER fired -> our boot does not
//   precede the game's swapchain creation, so that zero-ambiguity route is not
//   available and the ECL capture is what ships.
// OFF-edge: at confirmation (or at the tally cap, which HALTs) the summary is
// flushed and the hooks are hook::Disable'd -- patch lifted, trampoline slot
// retained on purpose (see ue_wrap/core/hook.h).
//
// Design of record:
// research/findings/tooling/votv-imgui-dx12-overlay-DESIGN-2026-07-26.md

#include "overlay_backend_internal.h"

#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdint>

namespace ui::overlay_backend::dx12_capture {
namespace {

constexpr int kTallyPresents    = 600;  // ambiguity cap: no confirmation by here -> HALT
constexpr int kConfirmPresents  = 30;   // confirmation window
constexpr int kConfirmMinWins   = 27;   // >=90% agreement (measured on the rig: 100%)

ID3D12Device* g_device = nullptr;          // AddRef'd at detection; the device-match anchor
std::atomic<bool> g_detectLogged{false};   // one-shot detection block
std::atomic<bool> g_eclHookInstalled{false};  // published by the worker, read by the render thread
bool g_halted = false;                     // ambiguity cap reached (render-thread owned)
std::atomic<bool> g_armFailed{false};      // worker could not arm the hook (terminal)
ID3D12CommandQueue* g_confirmed = nullptr; // the confirmed queue, latched (render-thread owned)
ID3D12CommandQueue* g_prevConfirmed = nullptr;  // seeds the candidate after a re-arm
bool g_reseed = false;
int  g_presents = 0;

using EclFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
EclFn g_eclTrampoline = nullptr;
void* g_eclTarget = nullptr;

// CreateSwapChain probes (installed at boot from imgui_overlay::Init).
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*,
                                                      DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                             const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                             IDXGIOutput*, IDXGISwapChain1**);
CreateSwapChainFn        g_createScTrampoline     = nullptr;
CreateSwapChainForHwndFn g_createScHwndTrampoline = nullptr;
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
    // Payload FIRST, publish SECOND: another thread's detour reads desc/
    // deviceMatch as soon as it sees a non-null q, and a half-written slot
    // would classify a COPY queue as DIRECT (type DIRECT == 0 zero-init;
    // perf audit MED-3, 2026-07-26).
    const D3D12_COMMAND_QUEUE_DESC desc = q->GetDesc();
    const DWORD tid = ::GetCurrentThreadId();
    bool deviceMatch = false;
    {
        ID3D12Device* qdev = nullptr;
        if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&qdev))) && qdev) {
            deviceMatch = (qdev == g_device);
            qdev->Release();
        }
    }
    for (int i = 0; i < kMaxQueues; ++i) {
        ID3D12CommandQueue* expected = nullptr;
        if (g_queues[i].q.load(std::memory_order_relaxed) == q) return &g_queues[i];
        QueueSlot& sPre = g_queues[i];
        if (sPre.q.load(std::memory_order_relaxed) == nullptr) {
            sPre.desc = desc;
            sPre.firstTid = tid;
            sPre.deviceMatch = deviceMatch;
        }
        if (g_queues[i].q.compare_exchange_strong(expected, q, std::memory_order_acq_rel)) {
            QueueSlot& s = g_queues[i];
            int cnt = g_queueCount.load(std::memory_order_relaxed);
            while (cnt <= i && !g_queueCount.compare_exchange_weak(cnt, i + 1,
                                                                   std::memory_order_release)) {}
            UE_LOGI("imgui_overlay: dx12 capture: queue %p first seen -- type=%d prio=%d tid=%lu "
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
    g_eclTrampoline(q, numLists, lists);
}

// ---- creation probes ---------------------------------------------------------

void LogCreationDevice(const char* which, IUnknown* pDevice) {
    ID3D12CommandQueue* q = nullptr;
    HRESULT qi = E_NOINTERFACE;
    if (pDevice) qi = pDevice->QueryInterface(IID_PPV_ARGS(&q));
    UE_LOGI("imgui_overlay: dx12 capture: %s fired -- pDevice=%p QI(ID3D12CommandQueue) "
            "hr=0x%08lX queue=%p tid=%lu",
            which, static_cast<void*>(pDevice), qi, static_cast<void*>(q),
            ::GetCurrentThreadId());
    if (q) q->Release();
}

HRESULT STDMETHODCALLTYPE CreateScDetour(IDXGIFactory* self, IUnknown* pDevice,
                                         DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    LogCreationDevice("CreateSwapChain", pDevice);
    return g_createScTrampoline(self, pDevice, desc, out);
}

HRESULT STDMETHODCALLTYPE CreateScHwndDetour(IDXGIFactory2* self, IUnknown* pDevice, HWND hwnd,
                                             const DXGI_SWAP_CHAIN_DESC1* desc,
                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                             IDXGIOutput* restrict_, IDXGISwapChain1** out) {
    LogCreationDevice("CreateSwapChainForHwnd", pDevice);
    return g_createScHwndTrampoline(self, pDevice, hwnd, desc, fs, restrict_, out);
}

// Off the render thread: make a throwaway D3D12 device + DIRECT queue purely
// to read ID3D12CommandQueue's vtable, then arm the capture detour. This never
// runs inside the Present detour -- creating a device under DXGI's own lock is
// the shape that deadlocks a render thread. MinHook freezes threads itself, so
// installing from this worker is safe.
DWORD WINAPI EclHookThread(LPVOID) {
    using CreateDevFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    HMODULE d3d12 = ::LoadLibraryW(L"d3d12.dll");
    CreateDevFn create = d3d12 ? reinterpret_cast<CreateDevFn>(
                                     ::GetProcAddress(d3d12, "D3D12CreateDevice"))
                               : nullptr;
    if (!create) {
        UE_LOGW("imgui_overlay: dx12 capture: D3D12CreateDevice unavailable -- ECL hook skipped");
        g_armFailed.store(true, std::memory_order_release);
        return 0;
    }
    ID3D12Device* dummyDev = nullptr;
    if (FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dummyDev))) || !dummyDev) {
        UE_LOGW("imgui_overlay: dx12 capture: dummy D3D12 device failed -- ECL hook skipped");
        g_armFailed.store(true, std::memory_order_release);
        return 0;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* dummyQ = nullptr;
    if (SUCCEEDED(dummyDev->CreateCommandQueue(&qd, IID_PPV_ARGS(&dummyQ))) && dummyQ) {
        void** vtbl = *reinterpret_cast<void***>(dummyQ);
        g_eclTarget = vtbl[10];  // ID3D12CommandQueue::ExecuteCommandLists
        if (ue_wrap::hook::Install(g_eclTarget, reinterpret_cast<void*>(&EclDetour),
                                   reinterpret_cast<void**>(&g_eclTrampoline))) {
            g_eclHookInstalled.store(true, std::memory_order_release);  // after the hook is live
            UE_LOGI("imgui_overlay: dx12 capture: ExecuteCommandLists hook armed (@%p)",
                    g_eclTarget);
            ue_wrap::log::Flush();
        } else {
            g_eclTarget = nullptr;
            g_armFailed.store(true, std::memory_order_release);
        }
        dummyQ->Release();
    } else {
        g_armFailed.store(true, std::memory_order_release);
    }
    dummyDev->Release();
    if (g_armFailed.load(std::memory_order_acquire))
        UE_LOGW("imgui_overlay: RHI = DX12: could not arm the presenting-queue capture -- "
                "the overlay stays down this run.");
    ue_wrap::log::Flush();
    return 0;
}

void LogDetectionOnce(IDXGISwapChain* sc) {
    if (g_detectLogged.exchange(true)) return;
    ID3D12Device* dev = nullptr;
    const HRESULT hrDev = sc->GetDevice(IID_PPV_ARGS(&dev));           // P1
    IDXGISwapChain3* sc3 = nullptr;
    const HRESULT hrSc3 = sc->QueryInterface(IID_PPV_ARGS(&sc3));      // P3
    DXGI_SWAP_CHAIN_DESC desc{};
    const HRESULT hrDesc = sc->GetDesc(&desc);
    UE_LOGI("imgui_overlay: RHI = DX12 -- capturing the presenting queue "
            "(P1 GetDevice hr=0x%08lX dev=%p | P3 QI(IDXGISwapChain3) hr=0x%08lX | desc "
            "hr=0x%08lX bufferCount=%u format=%d swapEffect=%d windowed=%d)",
            hrDev, static_cast<void*>(dev), hrSc3, hrDesc, desc.BufferCount,
            static_cast<int>(desc.BufferDesc.Format), static_cast<int>(desc.SwapEffect),
            desc.Windowed ? 1 : 0);
    ue_wrap::log::Flush();  // INFO rides the CRT buffer (log.cpp flushes WARN/ERROR only);
                            // a gate line nobody can read is not a measurement
    if (sc3) sc3->Release();
    if (dev) g_device = dev;  // keep the AddRef: the device-match anchor
    if (!g_device) return;
    HANDLE th = ::CreateThread(nullptr, 0, &EclHookThread, nullptr, 0, nullptr);
    if (th) {
        ::CloseHandle(th);
    } else {
        g_armFailed.store(true, std::memory_order_release);
        UE_LOGW("imgui_overlay: RHI = DX12: capture worker could not start -- overlay stays down");
        ue_wrap::log::Flush();
    }
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
        UE_LOGI("imgui_overlay: dx12 capture SUMMARY: queue %p type=%d tid=%lu device-match=%d "
                "calls=%llu lastBeforePresent=%llu/%d (%.1f%%)",
                static_cast<void*>(q), static_cast<int>(s.desc.Type), s.firstTid,
                s.deviceMatch ? 1 : 0, static_cast<unsigned long long>(calls),
                static_cast<unsigned long long>(wins), g_presents,
                g_presents ? 100.0 * static_cast<double>(wins) / g_presents : 0.0);
    }
    UE_LOGI("imgui_overlay: dx12 capture SUMMARY: %d queue(s), %d presents tallied, foreign "
            "(non-candidate) ECL calls=%llu%s, dropped-regs=%llu -- hooks disarming "
            "(disable-only; trampoline slots retained)",
            n, g_presents, static_cast<unsigned long long>(foreignCalls),
            foreignCalls ? "" : " [WARN: corpus could not disagree -- extend the run in-world]",
            static_cast<unsigned long long>(g_droppedQueueRegs.load(std::memory_order_relaxed)));
    ue_wrap::log::Flush();
    if (g_eclHookInstalled.exchange(false)) ue_wrap::hook::Disable(g_eclTarget);
    if (g_createScTarget)     ue_wrap::hook::Disable(g_createScTarget);
    if (g_createScHwndTarget) ue_wrap::hook::Disable(g_createScHwndTarget);
}

}  // namespace

ID3D12Device* Device() { return g_device; }

ID3D12CommandQueue* TryConfirmQueue(IDXGISwapChain* sc) {
    if (g_halted) return nullptr;
    // Once confirmed, hand the SAME queue back on every call. Without this
    // latch a bring-up failure after confirmation re-entered the confirmation
    // path every present -- re-flushing the whole SUMMARY (n log lines + a
    // disk flush) and re-baking the ImGui font atlas each frame (perf audit
    // HIGH-2, 2026-07-26).
    if (g_confirmed) return g_confirmed;
    LogDetectionOnce(sc);
    if (!g_device) return nullptr;
    if (g_armFailed.load(std::memory_order_acquire)) { g_halted = true; return nullptr; }
    if (!g_eclHookInstalled.load(std::memory_order_acquire)) return nullptr;  // worker still arming

    ID3D12CommandQueue* winner = g_lastDirectSameDev.exchange(nullptr, std::memory_order_relaxed);
    ++g_presents;
    static ID3D12CommandQueue* s_candidate = nullptr;
    static int s_window = 0, s_wins = 0;
    if (g_reseed) {  // a re-arm starts from the queue we already trusted
        g_reseed = false;
        s_candidate = g_prevConfirmed;
        s_window = 0;
        s_wins = 0;
    }
    if (winner) {
        const int n = g_queueCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            if (g_queues[i].q.load(std::memory_order_relaxed) == winner) {
                g_queues[i].presentWins.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        if (winner != s_candidate) { s_candidate = winner; s_window = 0; s_wins = 0; }
        ++s_wins;
    }
    if (s_candidate) ++s_window;
    if (s_candidate && s_window >= kConfirmPresents) {
        if (s_wins >= kConfirmMinWins) {
            g_confirmed = s_candidate;
            g_prevConfirmed = s_candidate;
            FlushSummaryAndDisarm();  // the measured record + hook::Disable (patch lifted)
            UE_LOGI("imgui_overlay: dx12: presenting queue CONFIRMED %p (%d/%d frames) -- "
                    "bringing the renderer up", static_cast<void*>(g_confirmed), s_wins, s_window);
            ue_wrap::log::Flush();
            return g_confirmed;
        }
        s_candidate = nullptr; s_window = 0; s_wins = 0;  // re-candidate
    }
    if (g_presents >= kTallyPresents) {
        // The design's HALT: no queue held the window. Record the full measured
        // set and stop -- the renderer is never built on an ambiguous capture.
        g_halted = true;
        FlushSummaryAndDisarm();
        UE_LOGW("imgui_overlay: RHI = DX12: no presenting queue could be confirmed in %d frames "
                "-- overlay stays down this run (see the SUMMARY lines above).", kTallyPresents);
        ue_wrap::log::Flush();
    }
    return nullptr;
}

void InstallCreationProbe() {
    // Resolve the factory vtable off a throwaway factory. dxgi.dll is already
    // loaded (by the game and by our dummy-DX11 vtable resolver); go through
    // GetProcAddress so we add no import.
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE dxgi = ::GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) { UE_LOGW("imgui_overlay: creation probe skipped -- dxgi.dll not loaded"); return; }
    CreateFactoryFn createFactory =
        reinterpret_cast<CreateFactoryFn>(::GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (!createFactory) {
        UE_LOGW("imgui_overlay: creation probe skipped -- no CreateDXGIFactory1");
        return;
    }
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
                                reinterpret_cast<void**>(&g_createScTrampoline)))
        g_createScTarget = nullptr;
    if (g_createScHwndTarget &&
        !ue_wrap::hook::Install(g_createScHwndTarget,
                                reinterpret_cast<void*>(&CreateScHwndDetour),
                                reinterpret_cast<void**>(&g_createScHwndTrampoline)))
        g_createScHwndTarget = nullptr;
    UE_LOGI("imgui_overlay: swapchain-creation probe armed (CreateSwapChain=%p ForHwnd=%p)",
            g_createScTarget, g_createScHwndTarget);
}

void Rearm() {
    // A swapchain recreation invalidates the "this queue presents that chain"
    // fact. Re-enable the capture hook and require a fresh confirmation; the
    // previously confirmed queue is SEEDED as the candidate, so in the normal
    // case (same queue) this costs one confirmation window and no blink beyond
    // it. Design of record + correctness audit I-2.
    if (g_halted || !g_eclTarget) return;
    g_confirmed = nullptr;
    g_presents = 0;
    g_reseed = true;
    if (ue_wrap::hook::Enable(g_eclTarget))
        g_eclHookInstalled.store(true, std::memory_order_release);
}


}  // namespace ui::overlay_backend::dx12_capture
