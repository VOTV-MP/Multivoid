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
//   4. Surfaces: the overlay hosts TWO surfaces -- the F1 dev menu (ui::dev_menu,
//      interactive) and the tilde player list (ui::scoreboard). The scoreboard is
//      PASSIVE for clients (hold tilde, no cursor, keep playing) and INTERACTIVE for
//      the host (toggle tilde, cursor, clickable -- the action board). "Capture"
//      (cursor + input swallow + SetCursorPos no-op) follows whichever interactive
//      surface is up (the menu always; the scoreboard only for the host).
// DX12 is detected (GetDevice for ID3D11Device fails) and logged but not yet drawn.

#include "ui/imgui_overlay.h"

#include "ui/dev_menu.h"
#include "ui/scoreboard.h"
#include "ui/server_browser.h"
#include "ui/host_save_picker.h"
#include "ui/loading_screen.h"
#include "ui/console.h"
#include "ui/hud.h"
#include "ui/toast.h"
#include "ui/chat_input.h"
#include "ui/voice_panel.h"
#include "coop/social/chat_sync.h"
#include "coop/multiplayer_menu.h"
#include "coop/voice/voice_chat.h"
#include "coop/join_curtain.h"  // instant-world: the short curtain (full-viewport alpha-fade cover)
#include "coop/join_progress.h"
#include "coop/session_manager.h"
#include "coop/dev/perf_probe.h"
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
std::atomic<bool> g_visible{false};        // F1 dev menu shown
std::atomic<bool> g_scoreboard{false};     // player-list scoreboard shown (real tilde key)
std::atomic<bool> g_scoreboardForced{false};  // VOTVCOOP_SCOREBOARD_OPEN test override (survives focus reset)
std::atomic<bool> g_inFrame{false};        // render-thread inside the ImGui pass (shutdown waits on this)

// ---- surface state -----------------------------------------------------------
// Two surfaces share this overlay: the F1 dev menu (always interactive) and the
// tilde player list. The scoreboard is INTERACTIVE only for the host (the clickable
// action board, Phase 2); clients peek passively. "Capture" = take the cursor +
// swallow input + no-op UE4's recenter -- on whenever an interactive surface is up.
inline bool MenuOpen()    { return g_visible.load(std::memory_order_relaxed); }
inline bool ScoreOpen()   { return g_scoreboard.load(std::memory_order_relaxed) ||
                                   g_scoreboardForced.load(std::memory_order_relaxed); }
inline bool BrowserOpen() { return ui::server_browser::IsOpen(); }
inline bool PickerOpen()  { return ui::host_save_picker::IsOpen(); }
inline bool LoadingOpen() { return ui::loading_screen::IsOpen(); }
inline bool ConsoleOpen() { return ui::console::IsOpen(); }
inline bool ChatOpen()    { return ui::chat_input::IsOpen(); }
inline bool VoiceOpen()   { return ui::voice_panel::IsOpen(); }
// VOTV's native pause/ESC menu is up (render-thread-safe atomic; see multiplayer_menu).
// Gates the passive coop HUD + chat off so they never draw OVER the modal pause menu.
inline bool PauseMenuOpen() { return coop::multiplayer_menu::IsPauseMenuOpen(); }
inline bool AnyOpen()     { return MenuOpen() || ScoreOpen() || BrowserOpen() || PickerOpen() ||
                                   LoadingOpen() || ConsoleOpen() || ChatOpen() || VoiceOpen(); }
inline bool CaptureActive() {
    // Interactive surfaces take the cursor + input: the F1 menu, the server browser + Host-
    // Game save picker (Connect / Host / type an IP / pick a save), the loading screen (its
    // Cancel button), the console (its command input), the T-chat input bar (typed keys
    // must reach ImGui, not the game), and the V voice-settings panel (sliders + combos).
    // The host scoreboard is interactive too; the client scoreboard is a passive peek.
    return MenuOpen() || BrowserOpen() || PickerOpen() || LoadingOpen() || ConsoleOpen() ||
           ChatOpen() || VoiceOpen() || (ScoreOpen() && ui::scoreboard::LocalIsHost());
}

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

// While an interactive surface owns input, swallow UE4's per-tick cursor recenter
// so the single real OS cursor tracks the mouse instead of snapping to the center.
BOOL WINAPI SetCursorPosDetour(int x, int y) {
    if (CaptureActive()) return TRUE;
    return g_origSetCursorPos(x, y);
}

LRESULT CALLBACK WndProcDetour(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Losing focus (Alt-Tab) must drop the scoreboard: a background window never
    // receives the tilde WM_KEYUP, so the client hold-to-peek would stick open (and a
    // host's interactive board would keep input captured). Fall through to the game.
    if (msg == WM_KILLFOCUS) g_scoreboard.store(false, std::memory_order_relaxed);
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
    // Tilde (`/~, VK_OEM_3 -- the physical key above TAB) -> player list. HOST:
    // press toggles the interactive (clickable) board. CLIENT: hold to peek (down
    // shows, up hides). Either way swallow tilde so the game never acts on it.
    // (Swallowed from ImGui too -- it's our surface key, not ImGui's grave-accent.)
    //
    // Moved off TAB on 2026-06-03 (user req): VOTV binds TAB to the player
    // inventory -- a core, constantly-used action -- and our unconditional TAB
    // swallow took that key away. Tilde is free in VOTV (at most UE4's dev console,
    // which we have no reason to surface; the F1 menu is our dev surface), so taking
    // it costs nothing. VK_OEM_3 is the physical tilde-position key on every layout
    // (the Russian layout puts Ё there -- still the same scancode), so this is the
    // key left of "1" / above TAB regardless of the user's keyboard language.
    if (msg == WM_KEYDOWN && wParam == VK_OEM_3) {
        if (ui::scoreboard::LocalIsHost()) {
            if ((lParam & (1 << 30)) == 0)  // ignore auto-repeat while held
                g_scoreboard.store(!g_scoreboard.load(std::memory_order_relaxed), std::memory_order_relaxed);
        } else {
            g_scoreboard.store(true, std::memory_order_relaxed);
        }
        return 0;
    }
    if (msg == WM_KEYUP && wParam == VK_OEM_3) {
        if (!ui::scoreboard::LocalIsHost()) g_scoreboard.store(false, std::memory_order_relaxed);
        return 0;
    }
    // T -> open the chat input (v60, user req): only mid-session (chat is
    // meaningless solo) and only when no other surface owns input (typing 't'
    // into the browser's name field must not pop the chat). Swallow the press
    // so the game never acts on T and the input bar doesn't start with a "t".
    if (msg == WM_KEYDOWN && wParam == 'T' && !CaptureActive() && !PauseMenuOpen() &&
        coop::chat_sync::SessionActive()) {
        ui::chat_input::Open();
        return 0;
    }
    // V -> toggle the voice settings panel (user 2026-06-12 round 1: V opens voice
    // settings as its own surface; the scoreboard button is gone). Opens only while
    // voice runs (devices live) and no other surface owns typed input (typing 'v'
    // into chat/browser must not pop it); closes whenever the panel is up. The
    // toggle edges are swallowed so the game never acts on V. Surfaces with text
    // fields never coexist with the panel (they all gate on !CaptureActive() too),
    // so the close path cannot eat a typed letter.
    if (msg == WM_KEYDOWN && wParam == 'V' && (lParam & (1 << 30)) == 0) {
        if (VoiceOpen()) { ui::voice_panel::Close(); return 0; }
        if (!CaptureActive() && coop::voice_chat::Enabled()) {
            ui::voice_panel::Toggle();
            return 0;
        }
    }
    // ESC while the chat input is open: close it and FALL THROUGH to the game
    // (the pause menu opens normally -- the user-requested "ESC = chat gone,
    // user lands in the menu" behavior). After Close() the capture block below
    // no longer swallows this keydown (chat was the only capturing surface).
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE && ChatOpen()) {
        ui::chat_input::Close();
    }
    if (g_imguiReady.load(std::memory_order_acquire)) {
        // INVARIANT: ImGui must see the RELEASE of every key it saw pressed. A surface
        // can CLOSE on a key's WM_KEYDOWN (chat Enter-submit -> Close at chat_input.cpp;
        // the ESC-close just above) BETWEEN that key's down and its up -- so the down
        // reaches ImGui (capture still on) but, gated only on CaptureActive(), the up
        // would route to the GAME (capture now off), latching Enter/Escape DOWN in ImGui
        // forever -> the next InputText insta-submits/cancels (user 2026-06-13: "after
        // one message every input field insta-cancels"). Fix: feed key RELEASES + WM_CHAR
        // UNCONDITIONALLY. AddKeyEvent(key,false) for a key ImGui never saw down is a
        // harmless no-op; feeding alone never captures (the swallow + cursor-hide below
        // stay gated on CaptureActive, so the game still gets its releases when no surface
        // owns input). Root fix of the pairing invariant -- not a per-surface Close()
        // key-reset (that would race the render thread).
        const bool release = (msg == WM_KEYUP || msg == WM_SYSKEYUP || msg == WM_CHAR);
        if (CaptureActive() || release)
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

        if (CaptureActive()) {
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

// v59 launch toast: poll the async version-check verdict once, toast it, then
// render any live toasts. Ordinary function (NOT inside the __try -- C++ locals
// with destructors are illegal in an SEH frame, C2712).
void DrawToasts() {
    static bool sVersionToasted = false;
    if (!sVersionToasted) {
        bool outdated = false;
        const std::string line = coop::session_manager::LatestVersionLine(&outdated);
        if (!line.empty()) {
            // The outdated warning lingers (the user should not miss it); the
            // up-to-date note is a short courtesy.
            ui::toast::Push(line, outdated ? 25.0f : 8.0f, outdated);
            sVersionToasted = true;
        }
    }
    ui::toast::Render();
}

// SEH-guarded per-frame ImGui pass (render thread). A fault here must NOT take down
// the game's render thread -- swallow it and leave the menu hidden.
void RenderFrameGuarded() {
    __try {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();  // sets io.MousePos from the real OS cursor (WM_MOUSEMOVE / GetCursorPos)
        // Draw the ImGui software cursor only for interactive surfaces (F1 menu, or
        // the host scoreboard). The passive client scoreboard shows no cursor.
        ImGui::GetIO().MouseDrawCursor = CaptureActive();
        ImGui::NewFrame();

        // Always-on PASSIVE coop HUD (screen-projected nameplates + the chat/event
        // feed). Drawn FIRST so the interactive surfaces below sit ON TOP of it. It
        // never captures input -- nameplates use the background draw list, the chat
        // window is NoInputs -- so CaptureActive() excludes it and the player keeps
        // playing while it overlays. SUPPRESSED while VOTV's native pause/ESC menu is up:
        // our ImGui renders after the game in Present, so the passive HUD would otherwise
        // draw on TOP of the modal pause menu (user 2026-06-13: chat over the ESC menu).
        if (ui::hud::IsActive() && !PauseMenuOpen()) ui::hud::Render();

        if (MenuOpen())    ui::dev_menu::Render();
        if (ScoreOpen())   ui::scoreboard::Render();
        if (VoiceOpen())   ui::voice_panel::Render();
        if (BrowserOpen()) ui::server_browser::Render();
        if (PickerOpen())  ui::host_save_picker::Render();
        if (ChatOpen() && !PauseMenuOpen()) ui::chat_input::Render();
        // The console (bottom log panel) then the loading screen (centered) draw LAST, so the
        // connecting UI sits on top of everything during a join.
        if (ConsoleOpen()) ui::console::Render();
        // instant-world curtain: a full-viewport cover on the BACKGROUND draw list (over the game world,
        // BEHIND the loading panel below + every window above). Drawn unconditionally -- it no-ops when
        // inactive and fades out AFTER the loading panel closes at SnapshotComplete (so it can't be gated on
        // LoadingOpen). Its own alpha-fade clock runs off ImGui::GetTime().
        coop::join_curtain::Render();
        if (LoadingOpen()) ui::loading_screen::Render();
        DrawToasts();  // top-center notices (the v59 version toast) above everything

        ImGui::Render();
        if (g_rtv) {
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        UE_LOGE("imgui_overlay: SEH in render frame -- hiding surfaces to protect the render thread");
        g_visible.store(false, std::memory_order_relaxed);
        g_scoreboard.store(false, std::memory_order_relaxed);
        ui::voice_panel::Close();  // re-fault guard, same as the picker/loading below
        ui::server_browser::Close();
        // CLOSE the picker too: if its Render() faulted, leaving it open re-enters
        // Render() every frame and re-faults forever (audit HIGH-2 re-fault loop).
        ui::host_save_picker::Close();
        // Same re-fault guard for the loading screen (also resets join_progress so a
        // faulted join can't leave the connecting state stuck) and the console.
        ui::loading_screen::Close();
        ui::console::Close();
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
    // Perf probe: count every presented frame (the frame anchor for ms/frame).
    // Cheap relaxed increment when armed; a single bool load when off.
    coop::dev::perf_probe::NoteFrame();
    if (!g_imguiReady.load(std::memory_order_acquire)) {
        if (BringUpGuarded(sc)) g_imguiReady.store(true, std::memory_order_release);
        // else: DX12 / not-ready -> just present normally.
    }

    // Render when an interactive surface is open OR the always-on passive HUD has
    // something to show (a nameplate / chat line) -- so the HUD overlays during play
    // without needing F1/tilde. HudActive() is a lock-free pair of atomics.
    // Also render while the instant-world curtain is active: it must keep drawing for its 0.4s
    // alpha-fade AFTER the loading panel drops at SnapshotComplete (Complete() makes LoadingOpen
    // false in the same drain tick as BeginDismiss), else the fade is skipped and the cover snaps.
    // Added to the RENDER gate only (NOT AnyOpen, which also gates input/cursor) -- the curtain is
    // purely visual + non-interactive.
    if (g_imguiReady.load(std::memory_order_acquire) &&
        (AnyOpen() || ui::hud::IsActive() || coop::join_curtain::IsActive())) {
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
    // VOTVCOOP_SCOREBOARD_OPEN=1 starts the player list visible (the smoke can't
    // hold/press the tilde key) -- autonomous screenshot of the roster.
    char sbEnv[8] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_SCOREBOARD_OPEN", sbEnv, sizeof(sbEnv)) > 0 &&
        sbEnv[0] == '1') {
        // Forced (not g_scoreboard) so it survives the host losing focus to the
        // launching client window -- the WM_KILLFOCUS reset only clears the real tilde key.
        g_scoreboardForced.store(true, std::memory_order_relaxed);
        UE_LOGI("imgui_overlay: VOTVCOOP_SCOREBOARD_OPEN=1 -- scoreboard starts visible (screenshot test)");
    }
    // VOTVCOOP_BROWSER_OPEN=1 starts the MULTIPLAYER server browser visible (the
    // boot-to-menu screenshot can't click the injected button) -- autonomous proof.
    char brEnv[8] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_BROWSER_OPEN", brEnv, sizeof(brEnv)) > 0 &&
        brEnv[0] == '1') {
        ui::server_browser::Open();
        UE_LOGI("imgui_overlay: VOTVCOOP_BROWSER_OPEN=1 -- server browser starts visible (screenshot test)");
    }
    // VOTVCOOP_TEST_CONNECT_DIRECT=<host:port> autonomously simulates a browser
    // "Direct connect" click: queues a LanDirect client Config that the harness
    // RunPlayLoop consumes (TakePendingStart) + boots g_session -- proving the
    // browser->session boot path without a real click. TEST-ONLY (never set in play).
    char cdEnv[64] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_CONNECT_DIRECT", cdEnv, sizeof(cdEnv)) > 0 && cdEnv[0]) {
        coop::session_manager::ConnectDirect(cdEnv);
        UE_LOGI("imgui_overlay: VOTVCOOP_TEST_CONNECT_DIRECT=%s -- queued a browser-path session start (test)", cdEnv);
    }
    // VOTVCOOP_TEST_HOST_LOBBY=1 simulates a browser "Host Game" click: POST /v1/host
    // to the master -> P2P host session (the exact path DoHost() runs). TEST-ONLY.
    char hlEnv[8] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_HOST_LOBBY", hlEnv, sizeof(hlEnv)) > 0 && hlEnv[0] == '1') {
        coop::session_manager::HostLobby("Test Host", std::string(), /*locked=*/false, /*playersMax=*/4);
        UE_LOGI("imgui_overlay: VOTVCOOP_TEST_HOST_LOBBY=1 -- fired a browser-path HOST announce (test)");
    }
    // VOTVCOOP_TEST_JOIN_LOBBY=<lobbyId> simulates clicking a browser row's Connect:
    // POST /v1/join -> P2P client session (the exact path JoinLobby() runs). TEST-ONLY.
    char jlEnv[64] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_JOIN_LOBBY", jlEnv, sizeof(jlEnv)) > 0 && jlEnv[0]) {
        coop::session_manager::JoinLobby(jlEnv, jlEnv);  // lobbyId doubles as the display label in the test
        UE_LOGI("imgui_overlay: VOTVCOOP_TEST_JOIN_LOBBY=%s -- fired a browser-path JOIN (test)", jlEnv);
    }
    // VOTVCOOP_TEST_HOST_SAVE=<slot> simulates the Host-Game picker's "Host selected
    // save": HostWithSave({existing slot}) -> the harness LOADS <slot> then hosts (the
    // exact path the picker's DoHostExisting runs). VOTVCOOP_TEST_HOST_NEW=<name> is the
    // "New Game & Host" (story) path. TEST-ONLY (never set in play).
    char hsEnv[64] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_HOST_SAVE", hsEnv, sizeof(hsEnv)) > 0 && hsEnv[0]) {
        coop::session_manager::SaveChoice c;
        c.newGame = false;
        c.slot = hsEnv;
        coop::session_manager::HostWithSave(c, "Test Host", /*locked=*/false, /*playersMax=*/4);
        UE_LOGI("imgui_overlay: VOTVCOOP_TEST_HOST_SAVE=%s -- fired a picker HOST-WITH-SAVE (load existing, test)", hsEnv);
    }
    char hnEnv[64] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_HOST_NEW", hnEnv, sizeof(hnEnv)) > 0 && hnEnv[0]) {
        coop::session_manager::SaveChoice c;
        c.newGame = true;
        c.newName = hnEnv;
        c.mode = 0;  // story
        coop::session_manager::HostWithSave(c, "Test Host", /*locked=*/false, /*playersMax=*/4);
        UE_LOGI("imgui_overlay: VOTVCOOP_TEST_HOST_NEW=%s -- fired a picker HOST-WITH-SAVE (new story game, test)", hnEnv);
    }
    // VOTVCOOP_TEST_LOADING=1 forces the CLIENT connecting/loading state up (no real connect)
    // so the loading screen + the menu fade + the console can be screenshotted determin-
    // istically. Sets a partial determinate bar (1400/2313). TEST-ONLY -- the 90 s failsafe
    // (join_progress::MaybeTimeout) lifts it on its own.
    char ldEnv[8] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_TEST_LOADING", ldEnv, sizeof(ldEnv)) > 0 && ldEnv[0] == '1') {
        coop::join_progress::BeginConnect("Test Host");
        coop::join_progress::BeginSnapshot(2313);
        for (int i = 0; i < 1400; ++i) coop::join_progress::NotePropApplied();
        UE_LOGI("imgui_overlay: VOTVCOOP_TEST_LOADING=1 -- forced the loading state (1400/2313) for a screenshot (test)");
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
    g_scoreboard.store(false, std::memory_order_relaxed);
    g_scoreboardForced.store(false, std::memory_order_relaxed);
    ui::voice_panel::Close();
    ui::server_browser::Close();
    ui::host_save_picker::Close();
    ui::loading_screen::Close();
    ui::console::Close();
    ui::console::Shutdown();  // unregister the logger sink so no post-teardown log re-enters it
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
