// ui/imgui_overlay.cpp -- the Dear ImGui host over the game's swap chain. Init() AOB-resolves four
// engine-private functions and MinHooks them: the two per-RHI present choke points (the draw seam,
// FD3D11Viewport::PresentChecked and FD3D12Viewport::PresentInternal) and the two per-RHI viewport
// Resize functions (the render-target release bracket). No DXGI vtable entry is patched, so
// RivaTuner has nothing of ours to unlink and OBS's game capture sees our pixels (docs/ui.md). The
// first real present brings ImGui and the RHI backend up (ui/overlay_backend.h) and hooks the
// WndProc; each frame runs the ImGui pass and composites the open surfaces. Capture (the software
// cursor, the input swallow, the SetCursorPos no-op) follows whichever interactive surface is up.
// The diagnostics live in ui/overlay_diag.cpp, the test-only env arming in ui/overlay_test_arm.cpp.

#include "ui/imgui_overlay.h"
#include "ui/native_text_field.h"   // a focused native field claims the key first

#include "ui/overlay_backend.h"

#include "ui/dev_menu.h"
#include "ui/scoreboard.h"
#include "ui/server_browser.h"
#include "ui/boot_warning_dialog.h"
#include "ui/config_review_panel.h"
#include "ui/connect_failed_dialog.h"
#include "ui/host_save_picker.h"
#include "ui/loading_screen.h"
#include "ui/console.h"
#include "ui/hud.h"
#include "ui/net_stats_panel.h"
#include "ui/chat_input.h"
#include "ui/atlas_watch.h"
#include "ui/overlay_diag.h"
#include "ui/overlay_test_arm.h"
#include "ui/overlay_cursor.h"
#include "ui/fonts.h"
#include "ui/scale.h"
#include "ui/style.h"
#include "ui/voice_panel.h"
#include "coop/comms/chat_sync.h"
#include "ui/input_focus.h"  // SetOverlayCapturingText -- the hotkey-poller text-capture gate
#include "ui/multiplayer_menu.h"
#include "coop/voice/voice_chat.h"
#include "ui/join_curtain.h"  // instant-world: the short curtain (full-viewport alpha-fade cover)
#include "coop/session/join_progress.h"
#include "coop/dev/perf_probe.h"
#include "coop/dev/input_focus_probe.h"
#include "coop/dev/worldless_frames.h"
#include "coop/input/input_owner.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sig_scan.h"     // FindPattern -- the FD3D11Viewport::PresentChecked seam probe
#include "ue_wrap/core/sdk_profile.h"  // the four seam signatures

#include <windows.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"

// ImGui's Win32 backend message handler (defined in imgui_impl_win32.cpp).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace ui::imgui_overlay {
namespace {

int g_seamsArmed = 0;   // how many of the four engine seams installed (for the boot line)

// user32!SetCursorPos, hooked to no-op UE4's per-tick cursor recentre while a surface captures, so
// the cursor can track the mouse over the menu.
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
SetCursorPosFn  g_setCursorPosTrampoline  = nullptr;
void*           g_setCursorPosTarget = nullptr;

HWND    g_hwnd    = nullptr;
WNDPROC g_origWndProc = nullptr;

std::atomic<bool> g_installed{false};   // hooks installed
std::atomic<bool> g_imguiReady{false};  // first-present init done (backend live)
std::atomic<bool> g_visible{false};        // F1 dev menu shown
std::atomic<bool> g_scoreboard{false};     // player-list scoreboard shown (real tilde key)
std::atomic<bool> g_scoreboardForced{false};  // VOTVCOOP_SCOREBOARD_OPEN test override (survives focus reset)

// ---- surface state ----
// The F1 dev menu is always interactive; the tilde player list is interactive for the host (the
// action board) and a passive peek for clients. Capture = the cursor, the input swallow and the
// recentre no-op, on whenever an interactive surface is up.
inline bool MenuOpen()    { return g_visible.load(std::memory_order_relaxed); }
inline bool ScoreOpen()   { return g_scoreboard.load(std::memory_order_relaxed) ||
                                   g_scoreboardForced.load(std::memory_order_relaxed); }
inline bool BrowserOpen() { return ui::server_browser::IsOpen(); }
inline bool PickerOpen()  { return ui::host_save_picker::IsOpen(); }
inline bool LoadingOpen() { return ui::loading_screen::IsOpen(); }
inline bool ConsoleOpen() { return ui::console::IsOpen(); }
inline bool ChatOpen()    { return ui::chat_input::IsOpen(); }
inline bool VoiceOpen()   { return ui::voice_panel::IsOpen(); }
inline bool ConnectFailedOpen() { return ui::connect_failed_dialog::IsOpen(); }
inline bool BootWarningOpen()   { return ui::boot_warning_dialog::IsOpen(); }
inline bool ConfigReviewOpen()  { return ui::config_review_panel::IsOpen(); }
// VOTV's native pause menu is up (a render-thread-safe atomic in multiplayer_menu); the passive HUD
// and the chat never draw over it.
inline bool PauseMenuOpen() { return coop::multiplayer_menu::IsPauseMenuOpen(); }
inline bool AnyOpen()     { return MenuOpen() || ScoreOpen() || BrowserOpen() || PickerOpen() ||
                                   LoadingOpen() || ConsoleOpen() || ChatOpen() || VoiceOpen() ||
                                   ConnectFailedOpen() || BootWarningOpen() || ConfigReviewOpen(); }
inline bool CaptureActive() {
    // The surfaces that take the cursor and input; the host scoreboard is one, the client
    // scoreboard is a passive peek.
    return MenuOpen() || BrowserOpen() || PickerOpen() || LoadingOpen() || ConsoleOpen() ||
           ChatOpen() || VoiceOpen() || ConnectFailedOpen() || BootWarningOpen() ||
           ConfigReviewOpen() || (ScoreOpen() && ui::scoreboard::LocalIsHost());
}

// While an interactive surface owns input, swallow UE4's per-tick cursor recentre so the one OS
// cursor tracks the mouse.
BOOL WINAPI SetCursorPosDetour(int x, int y) {
    ui::overlay_diag::NoteSetCursorPos(x, y);
    // Before the swallow: a suppressed write still proves the game is mouselooking, which is what
    // decides whether a capture transition has pointer ownership to hand back
    // (ui/overlay_cursor.h).
    ui::overlay_cursor::NoteGameCursorWrite();
    if (CaptureActive()) return TRUE;
    return g_setCursorPosTrampoline(x, y);
}

// The gate state the key trace reports, handed over only once the probe is armed: three predicate
// evaluations per key message would otherwise feed a line nobody prints.
inline void ProbeKeyMsg(UINT msg, WPARAM wParam, const char* verdict) {
    if (!coop::dev::input_focus_probe::IsArmed()) return;
    ui::overlay_diag::NoteKeyMsg(msg, wParam, verdict,
                                 {CaptureActive(), ChatOpen(), PauseMenuOpen()});
}

LRESULT CALLBACK WndProcDetour(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ui::overlay_diag::NoteWndProcMsg(msg);
    ui::overlay_diag::NoteWndProcThread();
    // Losing focus drops the scoreboard: a background window never receives the tilde WM_KEYUP, so
    // a hold-to-peek would stick open. Falls through to the game.
    if (msg == WM_KILLFOCUS) g_scoreboard.store(false, std::memory_order_relaxed);
    // F1 toggles the menu; the key is consumed. State, not the ShowCursor counter, drives the
    // cursor: VOTV shows the OS cursor during play, and the SetCursorPos no-op lets it track.
    if (msg == WM_KEYDOWN && wParam == VK_F1 &&
        (MenuOpen() || coop::input::input_owner::MayTakeKey(VK_F1))) {
        g_visible.store(!g_visible.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return 0;
    }
    // Tilde (VK_OEM_3, the physical key above TAB on every layout) opens the player list: the host
    // toggles the interactive board, a client holds to peek. Swallowed from the game and from
    // ImGui. Not TAB, which is the game's inventory key. On a fresh install this key also opens
    // UE4's developer console (DefaultInput.ini ConsoleKeys=Tilde), which the swallow takes away.
    if (msg == WM_KEYDOWN && wParam == VK_OEM_3 &&
        (ScoreOpen() || coop::input::input_owner::MayTakeKey(VK_OEM_3))) {
        if (ui::scoreboard::LocalIsHost()) {
            if ((lParam & (1 << 30)) == 0)  // ignore auto-repeat while held
                g_scoreboard.store(!g_scoreboard.load(std::memory_order_relaxed), std::memory_order_relaxed);
        } else {
            g_scoreboard.store(true, std::memory_order_relaxed);
        }
        return 0;
    }
    if (msg == WM_KEYUP && wParam == VK_OEM_3 &&
        (ScoreOpen() || coop::input::input_owner::MayTakeKey(VK_OEM_3))) {
        if (!ui::scoreboard::LocalIsHost()) g_scoreboard.store(false, std::memory_order_relaxed);
        return 0;
    }
    // T opens the chat input, only mid-session and only when no surface owns input (typing 't' into
    // the browser's name field must not pop the chat); the press is swallowed so the bar does not
    // start with a "t". Known gap: !CaptureActive() means the loading screen and the host
    // scoreboard, which own input but no text, still swallow T, so chat is unreachable during a
    // join; curing it means splitting CaptureActive by whether the surface takes typed text.
    if (msg == WM_KEYDOWN && wParam == 'T' && !CaptureActive() && !PauseMenuOpen() &&
        coop::input::input_owner::MayTakeKey('T') && coop::chat_sync::SessionActive()) {
        ProbeKeyMsg(msg, wParam, "SWALLOWED by the T-chat hotkey");
        ui::chat_input::Open();
        return 0;
    }
    // V toggles the voice settings panel: opens only while voice runs and no surface owns typed
    // input, closes whenever the panel is up; both edges are swallowed. Surfaces with text fields
    // never coexist with the panel, so the close path cannot eat a typed letter.
    if (msg == WM_KEYDOWN && wParam == 'V' && (lParam & (1 << 30)) == 0) {
        if (VoiceOpen()) { ui::voice_panel::Close(); return 0; }
        if (!CaptureActive() && coop::input::input_owner::MayTakeKey('V') &&
            coop::voice_chat::Enabled()) {
            ui::voice_panel::Toggle();
            return 0;
        }
    }
    // ESC with the chat open closes it and falls through, so the pause menu opens as it normally
    // would; after Close() the capture block below no longer swallows this keydown.
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE && ChatOpen()) {
        ui::chat_input::Close();
    }
    if (g_imguiReady.load(std::memory_order_acquire)) {
        // ImGui must see the release of every key it saw pressed: a surface can close on a key's
        // WM_KEYDOWN (chat's Enter-submit, the ESC-close above), and an up routed to the game would
        // latch that key down in ImGui, so every later InputText insta-submits. Releases and
        // WM_CHAR are fed unconditionally; feeding alone never captures, since the swallow below
        // stays gated.
        const bool release = (msg == WM_KEYUP || msg == WM_SYSKEYUP || msg == WM_CHAR);
        if (CaptureActive() || release)
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

        if (CaptureActive()) {
            // Force-hide the OS cursor over the client area (ImGui draws its own): SetCursor(NULL)
            // wins regardless of UE4's ShowCursor count, and TRUE halts further WM_SETCURSOR
            // processing.
            if (msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) { ::SetCursor(nullptr); return TRUE; }
            // Swallow the input the game would act on, WM_INPUT included: it is UE4's raw-input
            // mouselook feed, and the camera would spin under the menu.
            switch (msg) {
                case WM_INPUT:
                case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
                case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
                case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                    ProbeKeyMsg(msg, wParam, "SWALLOWED by CaptureActive");
                    return 1;
                default: break;
            }
        }
    }
    // A focused native text field takes the key here, the only place it can: the native screens are
    // hand-wired widget trees that Slate never routes keystrokes into (a UEditableTextBox with
    // keyboard focus read its Text back empty after two WM_CHARs, measured). Below the
    // CaptureActive block on purpose, in the gap it leaves: a native screen up and no ImGui
    // surface. Escape is only half handled here; the screens poll the physical key and ask
    // ConsumeEscape() at their edge.
    if (msg == WM_CHAR && ui::native_text_field::OnChar(static_cast<wchar_t>(wParam))) {
        ProbeKeyMsg(msg, wParam, "SWALLOWED by a native text field");
        return 1;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
        ui::native_text_field::OnKeyDown(static_cast<int>(wParam))) {
        ProbeKeyMsg(msg, wParam, "SWALLOWED by a native text field");
        return 1;
    }

    if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR ||
        msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
        ProbeKeyMsg(msg, wParam, "passed to the GAME");
    return ::CallWindowProcW(g_origWndProc, hwnd, msg, wParam, lParam);
}

// First-present bring-up; true once ImGui and the RHI backend are live. On any failure it releases
// what it acquired this call.
bool BringUp(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow) return false;

    if (!overlay_backend::CaptureDevice(sc)) return false;

    bool ctxCreated = false;
    if (!ImGui::GetCurrentContext()) { ImGui::CreateContext(); ctxCreated = true; }
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;     // don't litter a layout .ini next to the game
    io.MouseDrawCursor = true;    // ImGui draws its own cursor; the WM_SETCURSOR hide and the
                                  // SetCursorPos no-op keep it the only one, and tracking
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // arrow-key move + Enter/Space activate
    // Resolution scale: seeded from the client size before the first bake, so fonts and style are
    // born at this resolution's size (ui/scale.h).
    ui::scale::LoadUserPrefOnce();  // ini ui.scale (the F1 "UI size" pref)
    RECT rc{};
    if (::GetClientRect(desc.OutputWindow, &rc))
        ui::scale::NoteViewport(static_cast<float>(rc.right - rc.left),
                                static_cast<float>(rc.bottom - rc.top));
    ui::scale::ConsumeRebuild();  // the initial bake right here applies it
    // Reset, then scale (ui::style::MaybeRescale's shape): ScaleAllSizes is cumulative, and a
    // SEH-swallowed half bring-up can re-enter with an already scaled style.
    ui::style::Rebuild();
    if (!ImGui_ImplWin32_Init(desc.OutputWindow)) {
        UE_LOGE("imgui_overlay: ImGui_ImplWin32_Init failed");
        if (ctxCreated) { ImGui::DestroyContext(); ui::fonts::OnContextDestroyed();
                          ui::atlas_watch::OnContextDestroyed(); }
        overlay_backend::AbandonCapture();
        return false;
    }
    if (!overlay_backend::InitRenderer(sc)) {
        ImGui_ImplWin32_Shutdown();
        if (ctxCreated) { ImGui::DestroyContext(); ui::fonts::OnContextDestroyed();
                          ui::atlas_watch::OnContextDestroyed(); }
        overlay_backend::AbandonCapture();
        return false;
    }

    // Fonts after the renderer, as an invariant: ImFontAtlasBuildMain samples RendererHasTextures,
    // set inside InitRenderer, at the instant it runs, and a build before it would lock in an eager
    // atlas under the dynamic regime. Load() builds nothing today; the order makes the hazard
    // impossible rather than merely absent.
    ui::fonts::Load();

    g_hwnd = desc.OutputWindow;
    g_origWndProc = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProcDetour)));
    UE_LOGI("imgui_overlay: %s bring-up OK (hwnd=%p) -- F1 toggles the menu",
            overlay_backend::Kind(), g_hwnd);
    return true;
}


// The SEH-guarded per-frame ImGui pass (render thread): a fault hides the surfaces rather than
// taking down the render thread.
void RenderFrameGuarded(IDXGISwapChain* sc) {
    __try {
        ui::style::MaybeRescale(g_hwnd);
        overlay_backend::NewFrame();
        ImGui_ImplWin32_NewFrame();  // sets io.MousePos from the real OS cursor (WM_MOUSEMOVE / GetCursorPos)
        // The software cursor only for interactive surfaces; the passive client scoreboard shows
        // none.
        ImGui::GetIO().MouseDrawCursor = CaptureActive();
        ImGui::NewFrame();

        // The atlas is lazy (it grows, repacks and discards bakes while the game runs), and this is
        // the one site that watches it. Inside the frame, since an out-of-frame atlas query poisons
        // TexIsBuilt, and at the start, so it measures what the previous frame did.
        ui::atlas_watch::OnFrame();

        // The always-on passive HUD (nameplates, the chat and event feed), drawn first so the
        // surfaces sit on top; it never captures input. Suppressed under the native pause menu,
        // which our pass would otherwise draw over.
        if (ui::hud::IsActive() && !PauseMenuOpen()) ui::hud::Render();
        // The network-stats overlay (F1 > Network > Stats, off by default): passive, suppressed
        // under the pause menu, independent of hud::IsActive() (a solo host sees it too).
        if (ui::net_stats_panel::Enabled() && !PauseMenuOpen()) ui::net_stats_panel::Render();

        if (MenuOpen())    ui::dev_menu::Render();
        if (ScoreOpen())   ui::scoreboard::Render();
        if (VoiceOpen())   ui::voice_panel::Render();
        if (BrowserOpen()) ui::server_browser::Render();
        // The connect-failed modal draws after the browser, which a failed join reopens, so it
        // layers on top.
        if (ui::connect_failed_dialog::IsOpen()) ui::connect_failed_dialog::Render();
        // The config review (the settings check): persistent until dismissed, under the
        // boot-warning modal and over the regular surfaces.
        if (ConfigReviewOpen()) ui::config_review_panel::Render();
        // The boot-warning modal (an install problem found at boot) layers over whatever is up
        // until acknowledged.
        if (ui::boot_warning_dialog::IsOpen()) ui::boot_warning_dialog::Render();
        if (PickerOpen())  ui::host_save_picker::Render();
        if (ChatOpen() && !PauseMenuOpen()) ui::chat_input::Render();
        // The console, then the loading screen, draw last, so the connecting UI sits on top during
        // a join.
        if (ConsoleOpen()) ui::console::Render();
        // The join curtain: a full-viewport cover on the background draw list, drawn
        // unconditionally (it no-ops when inactive) because its fade runs after the loading panel
        // closes at SnapshotComplete.
        coop::join_curtain::Render();
        if (LoadingOpen()) ui::loading_screen::Render();

        // Publish "the overlay is capturing typed text" for the hotkey pollers (voice PTT, freecam,
        // the spawn menu), whose GetAsyncKeyState reads must not fire on a keystroke meant for a
        // field. WantTextInput, or the chat-open latch for the one focus-handoff frame.
        const bool overlayText = ImGui::GetIO().WantTextInput || ui::chat_input::IsOpen();
        ui::input_focus::SetOverlayCapturingText(overlayText);
        coop::input::input_owner::PublishOverlayOwnsText(overlayText);

        // The cursor ownership transition (MTA's CLocalGUI::Draw shape), before the probe so it
        // observes the state the frame draws with.
        ui::overlay_cursor::FrameTransition(g_hwnd, CaptureActive(), g_setCursorPosTrampoline);
        ui::overlay_diag::CursorFrame(g_hwnd, CaptureActive(), g_setCursorPosTrampoline);
        ImGui::Render();
        overlay_backend::RenderDrawData(sc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        UE_LOGE("imgui_overlay: SEH in render frame -- hiding surfaces to protect the render thread");
        g_visible.store(false, std::memory_order_relaxed);
        g_scoreboard.store(false, std::memory_order_relaxed);
        ui::voice_panel::Close();  // re-fault guard, same as the picker/loading below
        ui::server_browser::Close();
        // The connect-failed modal's pending reason is its open flag: clear it, or a faulted Render
        // re-enters every frame.
        coop::join_progress::ClearFailReason();
        // The boot-warning modal's pending text is its open flag, the same guard.
        ui::boot_warning_dialog::Clear();
        // The picker too: left open, a faulted Render re-enters every frame.
        ui::host_save_picker::Close();
        // The loading screen (which also resets join_progress, so a faulted join does not stay
        // connecting) and the console.
        ui::loading_screen::Close();
        ui::console::Close();
        // Unlatch the text-capture publish: a fault above skips the per-frame publish, and with the
        // chat open at the fault every gated hotkey would stay dead. Close the field and publish
        // keys-live, the frame's normal end state.
        ui::chat_input::Close();
        ui::input_focus::SetOverlayCapturingText(false);
        coop::input::input_owner::PublishOverlayOwnsText(false);
    }
}

// SEH-guarded first-present bring-up (also on the render thread).
bool BringUpGuarded(IDXGISwapChain* sc) {
    __try { return BringUp(sc); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        UE_LOGE("imgui_overlay: SEH during ImGui bring-up -- overlay disabled this run");
        return false;
    }
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
                    "pointer (docs/OVERLAY_CAPTURE_COEXIST.md, re-derive per "
                    "docs/VERSION_MIGRATION.md).", which, off, static_cast<void*>(sc));
            ue_wrap::log::Flush();
        }
        return nullptr;
    }
    lastOk = sc;
    return sc;
}

// One frame of overlay work; the swapchain arrives from whichever present seam fired.
void OverlayFrame(IDXGISwapChain* sc) {
    // The perf probe's frame count (the anchor for ms/frame): a relaxed increment when armed, a
    // bool load when off. It is also the instrument that shows a present seam unlinked (fps=0 while
    // everything else ticks), so it stays on this path.
    coop::dev::perf_probe::NoteFrame();
    // Time the whole detour body up to, not including, the engine's own present, into the
    // OverlayPresent bucket; a perf_probe::Scope would fire after the present returns and swallow
    // the vsync wait.
    const bool perfOn = coop::dev::perf_probe::Armed();
    const unsigned long long perfT0 = perfOn ? coop::dev::perf_probe::NowTicks() : 0;
    // The input probe samples even with no surface open (the state in which the game owns text), so
    // it hangs off the present, not the ImGui pass.
    coop::dev::input_focus_probe::NoteFrame();
    // Count frames presented per WorldKind: the one place "does the game present frames when no
    // world exists" can be asked.
    coop::dev::worldless_frames::NoteFrame();
    // Republish "does the game own typed text" on two cadences (input_owner.h): the cheap path at
    // ~10 Hz, the GUObjectArray walk at ~1 Hz, both posted to the game thread because they call
    // UFunctions.
    {
        static ULONGLONG sNextFast = 0, sNextFull = 0;
        const ULONGLONG now = ::GetTickCount64();
        if (now >= sNextFast) {
            const bool full = now >= sNextFull;
            if (full) sNextFull = now + 1000;
            sNextFast = now + 100;
            ue_wrap::game_thread::Post([full] { coop::input::input_owner::TickGameThread(full); });
        }
    }
    if (!g_imguiReady.load(std::memory_order_acquire)) {
        if (BringUpGuarded(sc)) g_imguiReady.store(true, std::memory_order_release);
        // Not brought up yet: present normally.
    }

    // Render when a surface is open, the passive HUD has something to show, or the join curtain is
    // fading (it must keep drawing for its 0.4 s fade after the loading panel drops). The curtain
    // is in the render gate only, not AnyOpen, which also gates input and the cursor.
    if (g_imguiReady.load(std::memory_order_acquire) &&
        (AnyOpen() || ui::hud::IsActive() || coop::join_curtain::IsActive())) {
        overlay_backend::EnsureTarget(sc);  // recreate the render target after a resize
        RenderFrameGuarded(sc);
    }
    if (perfOn) {
        coop::dev::perf_probe::AddTicks(coop::dev::perf_probe::Bucket::OverlayPresent,
                                        coop::dev::perf_probe::NowTicks() - perfT0);
    }
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

uintptr_t __fastcall D3D11PresentCheckedDetour(void* viewport, int32_t syncInterval) {
    if (IDXGISwapChain* sc = ValidatedSwapChain(viewport, ue_wrap::profile::kD3D11Viewport_SwapChain,
                                                "FD3D11Viewport::PresentChecked",
                                                g_lastOkSc11))
        OverlayFrame(sc);
    return g_d3d11PresentTrampoline(viewport, syncInterval);
}

uintptr_t __fastcall D3D12PresentInternalDetour(void* viewport, int32_t syncInterval) {
    if (IDXGISwapChain* sc = ValidatedSwapChain(viewport, ue_wrap::profile::kD3D12Viewport_SwapChain,
                                                "FD3D12Viewport::PresentInternal",
                                                g_lastOkSc12))
        OverlayFrame(sc);
    return g_d3d12PresentTrampoline(viewport, syncInterval);
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
void EngineResizeBracket(const char* which, size_t scOff, IDXGISwapChain*& lastOk,
                         void* viewport, const char* sizeNote) {
    IDXGISwapChain* sc = ValidatedSwapChain(viewport, scOff, which, lastOk);
    if (sc && g_imguiReady.load(std::memory_order_acquire)) {
        overlay_backend::OnResizeRecreate(sc);
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
    overlay_backend::OnResizeRelease();   // the back buffers are about to be recreated
    g_d3d11ResizeTrampoline(viewport, sizeX, sizeY, bFullscreen, pixelFormat);
    char note[96];
    _snprintf_s(note, sizeof(note), _TRUNCATE, " (%ux%u fullscreen=%u fmt=%d)", sizeX, sizeY,
                bFullscreen, pixelFormat);
    EngineResizeBracket("FD3D11Viewport::Resize", ue_wrap::profile::kD3D11Viewport_SwapChain,
                        g_lastOkSc11, viewport, note);
}

void __fastcall D3D12ResizeDetour(void* viewport) {
    overlay_backend::OnResizeRelease();
    g_d3d12ResizeTrampoline(viewport);
    // No size in the line: ResizeInternal takes only `this`, so four numbers here would be register
    // residue.
    EngineResizeBracket("FD3D12Viewport::ResizeInternal",
                        ue_wrap::profile::kD3D12Viewport_SwapChain, g_lastOkSc12, viewport, "");
}

}  // namespace

bool Init() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    if (!ue_wrap::hook::Init()) { UE_LOGE("imgui_overlay: hook::Init failed"); return false; }
    // Both RHIs' seams are installed unconditionally: which one the game presents with is unknown
    // at Init, and the other signature simply does not resolve in a single-RHI build. A stale
    // signature is not fatal: the overlay keeps drawing and says loudly that resizes are
    // unprotected.
    {
        struct Seam { const char* name; const char* sig; void* detour; void** tramp; bool draws; };
        const Seam seams[] = {
            {"FD3D11Viewport::PresentChecked", ue_wrap::profile::kSigD3D11ViewportPresentChecked,
             reinterpret_cast<void*>(&D3D11PresentCheckedDetour),
             reinterpret_cast<void**>(&g_d3d11PresentTrampoline), true},
            {"FD3D12Viewport::PresentInternal", ue_wrap::profile::kSigD3D12ViewportPresentInternal,
             reinterpret_cast<void*>(&D3D12PresentInternalDetour),
             reinterpret_cast<void**>(&g_d3d12PresentTrampoline), true},
            {"FD3D11Viewport::Resize", ue_wrap::profile::kSigD3D11ViewportResize,
             reinterpret_cast<void*>(&D3D11ResizeDetour),
             reinterpret_cast<void**>(&g_d3d11ResizeTrampoline), false},
            {"FD3D12Viewport::ResizeInternal", ue_wrap::profile::kSigD3D12ViewportResizeInternal,
             reinterpret_cast<void*>(&D3D12ResizeDetour),
             reinterpret_cast<void**>(&g_d3d12ResizeTrampoline), false},
        };
        int drawArmed = 0;
        for (const Seam& sm : seams) {
            const uintptr_t at = ue_wrap::FindPattern(sm.sig);
            if (!at) {
                // Expected for the RHI this cook does not contain; zero draw seams is checked after
                // the loop.
                UE_LOGW("imgui_overlay: %s signature NOT found -- that seam is ABSENT this "
                        "run (docs/OVERLAY_CAPTURE_COEXIST.md).", sm.name);
                continue;
            }
            if (ue_wrap::hook::Install(reinterpret_cast<void*>(at), sm.detour, sm.tramp)) {
                ++g_seamsArmed;
                if (sm.draws) ++drawArmed;
                UE_LOGI("imgui_overlay: %s %s armed @%p (image+0x%llX)", sm.name,
                        sm.draws ? "DRAW seam" : "resize bracket",
                        reinterpret_cast<void*>(at),
                        static_cast<unsigned long long>(
                            at - reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr))));
            } else {
                UE_LOGE("imgui_overlay: %s resolved but MinHook Install FAILED", sm.name);
            }
        }
        // Fail closed on the draw seam only: with none there is no overlay at all, so this returns
        // false rather than install a WndProc hook that swallows keys for a UI that cannot appear.
        // A missing resize bracket is a degradation, reported above.
        if (drawArmed == 0) {
            UE_LOGE("imgui_overlay: NO DRAW SEAM. Neither FD3D11Viewport::PresentChecked nor "
                    "FD3D12Viewport::PresentInternal resolved, so the overlay cannot draw and "
                    "is DISABLED for this run. This is what a game recook looks like from "
                    "here -- re-derive the signatures per docs/VERSION_MIGRATION.md.");
            ue_wrap::log::Flush();
            return false;
        }
    }
    // Hook user32!SetCursorPos to neutralise UE4's cursor recentre while a surface captures;
    // non-fatal, the cursor just tracks less cleanly.
    if (HMODULE u32 = ::GetModuleHandleW(L"user32.dll")) {
        g_setCursorPosTarget = reinterpret_cast<void*>(::GetProcAddress(u32, "SetCursorPos"));
        if (g_setCursorPosTarget &&
            ue_wrap::hook::Install(g_setCursorPosTarget, &SetCursorPosDetour,
                                   reinterpret_cast<void**>(&g_setCursorPosTrampoline))) {
            UE_LOGI("imgui_overlay: SetCursorPos hook installed (@%p) -- UE4 cursor recenter is "
                    "neutralized while the menu is up so the OS cursor tracks the mouse", g_setCursorPosTarget);
        } else {
            UE_LOGW("imgui_overlay: could not hook SetCursorPos -- cursor may not track over the menu");
            g_setCursorPosTarget = nullptr;
        }
    }
    // The swapchain-creation timing probe (log only: whether our boot precedes the game's swapchain
    // creation).
    ui::overlay_backend::InstallCreationProbe();
    g_installed.store(true, std::memory_order_release);
    // Test-only env arming (tools/mp.py's entry into the UI surfaces), ui/overlay_test_arm.cpp;
    // inert unless a VOTVCOOP_* test variable is set.
    ui::overlay_test_arm::ArmFromEnv();
    UE_LOGI("imgui_overlay: draw seam installed on the ENGINE present path (%d of 4 "
            "engine seams armed) -- ImGui brings up on the first frame; press F1 "
            "in-game for the menu", g_seamsArmed);
    return true;
}

void SetVisible(bool visible) { g_visible.store(visible, std::memory_order_relaxed); }
// Forced, not g_scoreboard, so it survives the host losing focus to the launching client window
// (WM_KILLFOCUS clears only the real key).
void ForceScoreboardOpen() { g_scoreboardForced.store(true, std::memory_order_relaxed); }

std::string CaptureOwners() {
    // By value: a pointer into a function-local static would alias between two %s in one log line.
    // Mirrors CaptureActive() term for term; a term added there and not here stops this naming the
    // surface holding the mouse.
    std::string buf;
    auto add = [&buf](const char* n) {
        if (!buf.empty()) buf += '+';
        buf += n;
    };
    if (MenuOpen())          add("devMenu(F1)");
    if (BrowserOpen())       add("imguiBrowser");
    if (PickerOpen())        add("savePicker");
    if (LoadingOpen())       add("loading");
    if (ConsoleOpen())       add("console");
    if (ChatOpen())          add("chat");
    if (VoiceOpen())         add("voice");
    if (ConnectFailedOpen()) add("connectFailed");
    if (BootWarningOpen())   add("bootWarning");
    if (ConfigReviewOpen())  add("configReview");
    if (ScoreOpen() && ui::scoreboard::LocalIsHost()) add("scoreboard");
    return buf.empty() ? std::string("none") : buf;
}

// ---- process exit ----
// No teardown here: a dying process needs only to stop new detour entries, and hook::Shutdown's
// blanket disable does that for every patch; hook::Uninstall would corrupt the trampoline in place.

}  // namespace ui::imgui_overlay
