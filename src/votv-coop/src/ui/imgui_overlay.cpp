// ui/imgui_overlay.cpp -- see ui/imgui_overlay.h.
//
// Standard "overlay over a game's DXGI swapchain" technique:
//   1. Init(): make a throwaway DX11 device + swapchain on a hidden window ONLY to
//      read the IDXGISwapChain vtable, then MinHook Present (vtable[8]) +
//      ResizeBuffers (vtable[13]). The vtable is shared by every swapchain, so
//      hooking the dummy's entry hooks the game's real swapchain too.
//   2. PresentDetour: on the FIRST real present, the game's device + window already
//      exist -- capture them, bring up ImGui + the DX11 backend + a WndProc hook,
//      then each frame run the ImGui pass and draw the active UI surface.
//   3. WndProcDetour: F1 toggles; while visible, route input to ImGui + swallow it
//      so the game doesn't also act on it (we still eat WM_INPUT so UE4's raw-input
//      mouselook can't spin the camera behind the menu). CURSOR model: ImGui draws
//      its OWN software cursor (io.MouseDrawCursor=true) -- always visible regardless
//      of UE4's OS-cursor state (UE4 keeps the OS cursor HIDDEN during play, so a
//      bare OS cursor would be invisible). To track the real mouse we no-op
//      SetCursorPos while the menu is up (the SetCursorPos hook) so UE4 can't snap
//      the cursor back to the window center, and we force-hide the OS cursor on
//      WM_SETCURSOR so it can never become a second cursor. ONE visible cursor.
//      Keyboard nav (arrows + Enter) via ImGuiConfigFlags_NavEnableKeyboard.
// DX12 is detected (GetDevice for ID3D11Device fails) and logged but not yet drawn.

#include "ui/imgui_overlay.h"

#include "ui/dev_menu.h"
#include "ue_wrap/hook.h"
#include "ue_wrap/log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

// ImGui's Win32 backend message handler (defined in imgui_impl_win32.cpp).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace ui::imgui_overlay {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
// IDXGISwapChain::ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags).
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                                    DXGI_FORMAT, UINT);

PresentFn       g_origPresent = nullptr;
ResizeBuffersFn g_origResize  = nullptr;
void*           g_presentTarget = nullptr;
void*           g_resizeTarget  = nullptr;

// user32!SetCursorPos -- hooked so we can no-op UE4's per-tick cursor recentering
// while the menu is visible (otherwise the OS cursor is snapped back to the window
// center every frame and can't track the mouse over the menu).
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
SetCursorPosFn  g_origSetCursorPos  = nullptr;
void*           g_setCursorPosTarget = nullptr;

ID3D11Device*           g_device  = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
ID3D11RenderTargetView* g_rtv     = nullptr;
HWND                    g_hwnd    = nullptr;
WNDPROC                 g_origWndProc = nullptr;

std::atomic<bool> g_installed{false};   // hooks installed
std::atomic<bool> g_imguiReady{false};  // first-present init done (DX11)
std::atomic<bool> g_dx12Logged{false};  // logged the DX12-unsupported notice once
std::atomic<bool> g_visible{false};     // menu shown (F1)
std::atomic<bool> g_inFrame{false};     // render-thread inside the ImGui pass (shutdown waits on this)

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

// While the menu owns input, swallow UE4's per-tick cursor recenter so the single
// real OS cursor tracks the mouse instead of being snapped to the window center.
BOOL WINAPI SetCursorPosDetour(int x, int y) {
    if (g_visible.load(std::memory_order_relaxed)) return TRUE;
    return g_origSetCursorPos(x, y);
}

LRESULT CALLBACK WndProcDetour(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // F1 edge -> toggle the menu (consume the key so the game never sees F1). No
    // ShowCursor: VOTV already shows the OS cursor during play (it's the one that was
    // stuck at center); io.MouseDrawCursor=false + the SetCursorPos no-op simply lets
    // that single cursor track instead of being recentered. Touching the ShowCursor
    // counter here only risks drift across the non-F1 visibility paths (env-open, the
    // SEH render-fault reset, SetVisible) -- state, not a counter, drives the cursor.
    if (msg == WM_KEYDOWN && wParam == VK_F1) {
        g_visible.store(!g_visible.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return 0;
    }
    if (g_visible.load(std::memory_order_relaxed) && g_imguiReady.load(std::memory_order_acquire)) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
        // Force-hide the OS cursor over the client area: ImGui draws its own, so a
        // visible OS cursor would be a second one. SetCursor(NULL) wins regardless of
        // UE4's ShowCursor count; return TRUE halts further WM_SETCURSOR processing.
        if (msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) { ::SetCursor(nullptr); return TRUE; }
        // Swallow input the game would otherwise act on so clicks/keys go to ImGui.
        // WM_INPUT is swallowed too: it's UE4's raw-input mouselook feed -- if the
        // game saw it, the camera would spin while we move the mouse over the menu.
        switch (msg) {
            case WM_INPUT:
            case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                return 1;
            default: break;
        }
    }
    return ::CallWindowProcW(g_origWndProc, hwnd, msg, wParam, lParam);
}

// First-present bring-up. Returns true once ImGui+DX11 are live. On ANY failure it
// releases whatever it acquired this call (no leak across retries).
bool BringUpDX11(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow) return false;

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

    bool ctxCreated = false;
    if (!ImGui::GetCurrentContext()) { ImGui::CreateContext(); ctxCreated = true; }
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;     // don't litter a layout .ini next to the game
    io.MouseDrawCursor = true;    // ImGui draws its OWN cursor -- always visible (UE4 keeps
                                  // the OS cursor hidden during play); the WM_SETCURSOR
                                  // hide + SetCursorPos no-op keep it the only one + tracking
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // arrow-key move + Enter/Space activate
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(desc.OutputWindow)) {
        UE_LOGE("imgui_overlay: ImGui_ImplWin32_Init failed");
        if (ctxCreated) ImGui::DestroyContext();
        if (ctx) ctx->Release();
        dev->Release();
        return false;
    }
    if (!ImGui_ImplDX11_Init(dev, ctx)) {
        UE_LOGE("imgui_overlay: ImGui_ImplDX11_Init failed");
        ImGui_ImplWin32_Shutdown();
        if (ctxCreated) ImGui::DestroyContext();
        if (ctx) ctx->Release();
        dev->Release();
        return false;
    }

    // Commit the captured handles only after everything succeeded.
    g_device = dev; g_context = ctx; g_hwnd = desc.OutputWindow;
    CreateRTV(sc);
    g_origWndProc = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProcDetour)));
    UE_LOGI("imgui_overlay: DX11 bring-up OK (hwnd=%p device=%p) -- F1 toggles the menu",
            g_hwnd, g_device);
    return true;
}

// SEH-guarded per-frame ImGui pass (render thread). A fault here must NOT take down
// the game's render thread -- swallow it and leave the menu hidden.
void RenderFrameGuarded() {
    __try {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();  // sets io.MousePos from the real OS cursor (WM_MOUSEMOVE / GetCursorPos)
        ImGui::NewFrame();

        ui::dev_menu::Render();

        ImGui::Render();
        if (g_rtv) {
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        UE_LOGE("imgui_overlay: SEH in render frame -- hiding menu to protect the render thread");
        g_visible.store(false, std::memory_order_relaxed);
    }
}

// SEH-guarded first-present bring-up (also on the render thread).
bool BringUpGuarded(IDXGISwapChain* sc) {
    __try { return BringUpDX11(sc); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        UE_LOGE("imgui_overlay: SEH during ImGui bring-up -- overlay disabled this run");
        return false;
    }
}

HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!g_imguiReady.load(std::memory_order_acquire)) {
        if (BringUpGuarded(sc)) g_imguiReady.store(true, std::memory_order_release);
        // else: DX12 / not-ready -> just present normally.
    }

    if (g_imguiReady.load(std::memory_order_acquire) && g_visible.load(std::memory_order_relaxed)) {
        g_inFrame.store(true, std::memory_order_release);
        if (!g_rtv) CreateRTV(sc);  // recreate after a resize
        RenderFrameGuarded();
        g_inFrame.store(false, std::memory_order_release);
    }
    return g_origPresent(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE ResizeBuffersDetour(IDXGISwapChain* sc, UINT bufCount, UINT w,
                                              UINT h, DXGI_FORMAT fmt, UINT flags) {
    ReleaseRTV();  // the backbuffer is about to be recreated
    const HRESULT hr = g_origResize(sc, bufCount, w, h, fmt, flags);
    if (SUCCEEDED(hr) && g_imguiReady.load(std::memory_order_acquire)) CreateRTV(sc);
    else if (FAILED(hr)) UE_LOGW("imgui_overlay: ResizeBuffers failed (hr=0x%08lX) -- RTV left null", hr);
    return hr;
}

// Read IDXGISwapChain::Present (vtbl[8]) + ResizeBuffers (vtbl[13]) by spinning up a
// throwaway DX11 device + swapchain on a hidden window. Fills g_presentTarget/g_resizeTarget.
bool ResolveSwapChainVtable() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VOTVCoopImguiDummyWnd";
    ::RegisterClassExW(&wc);
    HWND dummy = ::CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                 nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummy) { ::UnregisterClassW(wc.lpszClassName, wc.hInstance); return false; }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummy;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                                 want, 2, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr) && sc) {
        void** vtbl = *reinterpret_cast<void***>(sc);
        g_presentTarget = vtbl[8];   // IDXGISwapChain::Present
        g_resizeTarget  = vtbl[13];  // IDXGISwapChain::ResizeBuffers
        ok = (g_presentTarget != nullptr && g_resizeTarget != nullptr);
    } else {
        UE_LOGW("imgui_overlay: dummy D3D11CreateDeviceAndSwapChain failed (hr=0x%08lX)", hr);
    }
    if (sc)  sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    ::DestroyWindow(dummy);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

}  // namespace

bool Init() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    if (!ue_wrap::hook::Init()) { UE_LOGE("imgui_overlay: hook::Init failed"); return false; }
    if (!ResolveSwapChainVtable()) {
        UE_LOGE("imgui_overlay: could not resolve the IDXGISwapChain vtable -- overlay disabled");
        return false;
    }
    const bool p = ue_wrap::hook::Install(g_presentTarget, &PresentDetour,
                                          reinterpret_cast<void**>(&g_origPresent));
    const bool r = ue_wrap::hook::Install(g_resizeTarget, &ResizeBuffersDetour,
                                          reinterpret_cast<void**>(&g_origResize));
    if (!p || !r) {
        UE_LOGE("imgui_overlay: MinHook Install failed (present=%d resize=%d)", p ? 1 : 0, r ? 1 : 0);
        return false;
    }
    // Hook user32!SetCursorPos so we can neutralize UE4's per-tick cursor recenter
    // while the menu is visible (non-fatal if it can't hook -- the menu still works,
    // the cursor just won't track as cleanly).
    if (HMODULE u32 = ::GetModuleHandleW(L"user32.dll")) {
        g_setCursorPosTarget = reinterpret_cast<void*>(::GetProcAddress(u32, "SetCursorPos"));
        if (g_setCursorPosTarget &&
            ue_wrap::hook::Install(g_setCursorPosTarget, &SetCursorPosDetour,
                                   reinterpret_cast<void**>(&g_origSetCursorPos))) {
            UE_LOGI("imgui_overlay: SetCursorPos hook installed (@%p) -- UE4 cursor recenter is "
                    "neutralized while the menu is up so the OS cursor tracks the mouse", g_setCursorPosTarget);
        } else {
            UE_LOGW("imgui_overlay: could not hook SetCursorPos -- cursor may not track over the menu");
            g_setCursorPosTarget = nullptr;
        }
    }
    g_installed.store(true, std::memory_order_release);
    // Autonomous screenshot test: VOTVCOOP_MENU_OPEN=1 starts the menu visible (the
    // smoke can't press F1). Win32 env read (no CRT getenv -- /W4-clean in a DLL).
    char menuEnv[8] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_MENU_OPEN", menuEnv, sizeof(menuEnv)) > 0 &&
        menuEnv[0] == '1') {
        g_visible.store(true, std::memory_order_relaxed);
        UE_LOGI("imgui_overlay: VOTVCOOP_MENU_OPEN=1 -- menu starts visible (screenshot test)");
    }
    UE_LOGI("imgui_overlay: present hook installed (present=%p resize=%p) -- ImGui brings up "
            "on the first frame; press F1 in-game for the menu", g_presentTarget, g_resizeTarget);
    return true;
}

bool IsVisible() { return g_visible.load(std::memory_order_relaxed); }
void SetVisible(bool visible) { g_visible.store(visible, std::memory_order_relaxed); }

void Shutdown() {
    if (!g_installed.exchange(false)) return;
    // Stop new ImGui work + remove the hooks so no new Present/WndProc routes to us.
    g_visible.store(false, std::memory_order_relaxed);
    const bool wasReady = g_imguiReady.exchange(false);
    if (g_presentTarget) ue_wrap::hook::Uninstall(g_presentTarget);
    if (g_resizeTarget)  ue_wrap::hook::Uninstall(g_resizeTarget);
    if (g_setCursorPosTarget) { ue_wrap::hook::Uninstall(g_setCursorPosTarget); g_setCursorPosTarget = nullptr; }
    // Wait out any render-thread frame in flight before tearing down the context
    // (MinHook re-arms the original but does not join in-flight detour calls).
    for (int i = 0; i < 200 && g_inFrame.load(std::memory_order_acquire); ++i) ::Sleep(1);
    if (wasReady) {
        if (g_hwnd && g_origWndProc)
            ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    ReleaseRTV();
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device)  { g_device->Release();  g_device = nullptr; }
}

}  // namespace ui::imgui_overlay
