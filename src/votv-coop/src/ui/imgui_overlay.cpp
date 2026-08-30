// ui/imgui_overlay.cpp -- see ui/imgui_overlay.h.
//
// SEAM NOTICE (2026-08-22): the Present-hook architecture described below is the
// measured ROOT of two user-reported failures -- the overlay is INVISIBLE when
// RivaTuner/MSI-Afterburner is running (RTSS's hook-integrity control restores its
// own bytes over ours and unlinks our detour), and OBS game-capture cannot see it
// (OBS copies the backbuffer at the top of ITS Present detour, before ours draws).
// SHIPPED 2026-08-30. Our draw now happens inside FD3D11Viewport::PresentChecked (and
// FD3D12Viewport::PresentInternal) -- the engine's own private callers of the swapchain
// Present, upstream of the whole external inline-hook chain -- and the
// IDXGISwapChain::Present + ResizeBuffers hooks and the dummy-swapchain vtable resolve
// they needed are RETIRED WHOLE (RULE 2). READ docs/OVERLAY_CAPTURE_COEXIST.md BEFORE
// changing anything about where or how this file draws.
//
// Standard "overlay over a game's DXGI swapchain" technique:
//   1. Init(): AOB-resolve FOUR engine-private functions and MinHook them -- the two
//      per-RHI present choke points (the DRAW seam) and the two per-RHI viewport
//      Resize functions (the render-target release bracket). No DXGI vtable entry is
//      patched, which is the entire point: those are the bytes RTSS restores.
//   2. The draw seam: on the FIRST real present, the game's device + window already
//      exist -- capture them, bring up ImGui + the RHI render backend + a WndProc
//      hook, then each frame run the ImGui pass and draw the active UI surface.
//      Everything that touches a concrete D3D device lives behind
//      ui/overlay_backend.h (overlay_backend_dx11.cpp today); this file owns the
//      hooks, the WndProc, and surface compositing only.
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
//   5. Both RHIs DRAW: the DX11/DX12 halves live behind ui/overlay_backend.h. (This
//      line said "DX12 is detected ... but not yet drawn" until 2026-07-31; the DX12
//      backend shipped 2026-07-26 and the comment never moved.)
// The two DIAGNOSTIC instruments this file used to carry -- the per-key-message trace
// and the cursor probe -- are in ui/overlay_diag.cpp; they are instruments ABOUT the
// overlay, not part of it, and they had pushed this file past the 800-LOC cap. The
// TEST-ONLY env-var arming blocks Init() used to carry (VOTVCOOP_MENU_OPEN et al.)
// are in ui/overlay_test_arm.cpp on the same argument (2026-08-28).

#include "ui/imgui_overlay.h"

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
#include "ue_wrap/core/sdk_profile.h"  // kSigD3D11ViewportPresentChecked (docs/OVERLAY_CAPTURE_COEXIST.md)

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

// RULE 2, 2026-08-30: `ResizeBuffersFn`, `g_resizeTrampoline` and `g_resizeTarget` were
// deleted with the IDXGISwapChain::ResizeBuffers hook they served. The resize bracket
// lives on the engine's own FD3D11Viewport::Resize / FD3D12Viewport::Resize now (see
// EngineResizeBracket below and docs/OVERLAY_CAPTURE_COEXIST.md section 9c commit 1) --
// there is ONE bracket, and it is not on a function RTSS unlinks.
//
// `PresentFn`, `g_presentTrampoline`, `g_presentTarget` and `ResolveSwapChainVtable` went
// the same way, with the IDXGISwapChain::Present hook: the draw seam is
// FD3D11Viewport::PresentChecked / FD3D12Viewport::PresentInternal now.

int g_seamsArmed = 0;   // how many of the four engine seams installed (for the boot line)

// user32!SetCursorPos -- hooked so we can no-op UE4's per-tick cursor recentering
// while the menu is visible (otherwise the OS cursor is snapped back to the window
// center every frame and can't track the mouse over the menu).
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
inline bool ConnectFailedOpen() { return ui::connect_failed_dialog::IsOpen(); }
inline bool BootWarningOpen()   { return ui::boot_warning_dialog::IsOpen(); }
inline bool ConfigReviewOpen()  { return ui::config_review_panel::IsOpen(); }
// VOTV's native pause/ESC menu is up (render-thread-safe atomic; see multiplayer_menu).
// Gates the passive coop HUD + chat off so they never draw OVER the modal pause menu.
inline bool PauseMenuOpen() { return coop::multiplayer_menu::IsPauseMenuOpen(); }
inline bool AnyOpen()     { return MenuOpen() || ScoreOpen() || BrowserOpen() || PickerOpen() ||
                                   LoadingOpen() || ConsoleOpen() || ChatOpen() || VoiceOpen() ||
                                   ConnectFailedOpen() || BootWarningOpen() || ConfigReviewOpen(); }
inline bool CaptureActive() {
    // Interactive surfaces take the cursor + input: the F1 menu, the server browser + Host-
    // Game save picker (Connect / Host / type an IP / pick a save), the loading screen (its
    // Cancel button), the console (its command input), the T-chat input bar (typed keys
    // must reach ImGui, not the game), and the V voice-settings panel (sliders + combos).
    // The host scoreboard is interactive too; the client scoreboard is a passive peek.
    return MenuOpen() || BrowserOpen() || PickerOpen() || LoadingOpen() || ConsoleOpen() ||
           ChatOpen() || VoiceOpen() || ConnectFailedOpen() || BootWarningOpen() ||
           ConfigReviewOpen() || (ScoreOpen() && ui::scoreboard::LocalIsHost());
}

// While an interactive surface owns input, swallow UE4's per-tick cursor recenter
// so the single real OS cursor tracks the mouse instead of snapping to the center.
BOOL WINAPI SetCursorPosDetour(int x, int y) {
    ui::overlay_diag::NoteSetCursorPos(x, y);
    // BEFORE the swallow, deliberately: a suppressed write still proves the game is
    // mouselooking, and that -- not `CaptureActive()` -- is what decides whether a
    // capture transition has any pointer ownership to hand back (ui/overlay_cursor.h).
    ui::overlay_cursor::NoteGameCursorWrite();
    if (CaptureActive()) return TRUE;
    return g_setCursorPosTrampoline(x, y);
}

// The gate state the key trace reports. These predicates are file-local, so the
// instrument is handed their values rather than reaching for them -- but only once the
// probe is armed, which it never is in a normal run. Reading them unconditionally would
// put three predicate evaluations on every key message to feed a line nobody prints.
inline void ProbeKeyMsg(UINT msg, WPARAM wParam, const char* verdict) {
    if (!coop::dev::input_focus_probe::IsArmed()) return;
    ui::overlay_diag::NoteKeyMsg(msg, wParam, verdict,
                                 {CaptureActive(), ChatOpen(), PauseMenuOpen()});
}

LRESULT CALLBACK WndProcDetour(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ui::overlay_diag::NoteWndProcMsg(msg);
    ui::overlay_diag::NoteWndProcThread();
    // Losing focus (Alt-Tab) must drop the scoreboard: a background window never
    // receives the tilde WM_KEYUP, so the client hold-to-peek would stick open (and a
    // host's interactive board would keep input captured). Fall through to the game.
    if (msg == WM_KILLFOCUS) g_scoreboard.store(false, std::memory_order_relaxed);
    // F1 edge -> toggle the menu (consume the key so the game never sees F1). No
    // ShowCursor: VOTV already shows the OS cursor during play (it's the one that was
    // stuck at center); the SetCursorPos no-op simply lets
    // that single cursor track instead of being recentered. (This comment said
    // "io.MouseDrawCursor=false" until 2026-07-31; that was FALSE against the code --
    // :288 sets it true and the per-frame line sets it to CaptureActive() -- and it
    // contradicted the file header, which says the opposite. Two theories of the cursor
    // lived in one file's comments while the cursor bug went unrooted.) Touching the ShowCursor
    // counter here only risks drift across the non-F1 visibility paths (env-open, the
    // SEH render-fault reset, SetVisible) -- state, not a counter, drives the cursor.
    if (msg == WM_KEYDOWN && wParam == VK_F1 &&
        (MenuOpen() || coop::input::input_owner::MayTakeKey(VK_F1))) {
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
    // swallow took that key away. VK_OEM_3 is the physical tilde-position key on every layout
    // (the Russian layout puts Ё there -- still the same scancode), so this is the
    // key left of "1" / above TAB regardless of the user's keyboard language.
    //
    // THIS KEY IS NOT FREE, and the comment here claimed it was until 2026-07-31 ("Tilde
    // is free in VOTV (at most UE4's dev console, which we have no reason to surface)").
    // MEASURED against the cooked DefaultInput.ini: `ConsoleKeys=Tilde` + `ConsoleKeys=Ñ`
    // -- on a FRESH install this key opens UE4's developer console, and we swallow it
    // unconditionally. (On this dev box the player has rebound it, so the live
    // %LOCALAPPDATA% Input.ini reads `ConsoleKeys=F10` and tilde really is free HERE --
    // which is exactly why a shipped default may not be checked against one machine.)
    // The default moves to F3 in the bind commit; see the DESIGN doc §5.
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
    // STILL BROKEN, and deliberately not fixed here (docs/LESSONS.md, "A readiness
    // ANNOUNCEMENT is not evidence of the VISIBLE state it precedes" -- cited by TITLE
    // because this said `:794-798` until 2026-07-31 and that range is a DIFFERENT
    // lesson): the
    // leading !CaptureActive() means LoadingOpen() and ScoreOpen() -- surfaces that own
    // input but own NO TEXT -- still swallow T whole, so chat is unreachable during a
    // join. The arbiter added below makes that expressible (it is the OverlayNonText
    // case) but does NOT yet cure it; curing it means splitting CaptureActive by
    // whether the surface takes typed text, which is its own change.
    //
    // T -> open the chat input (v60, user req): only mid-session (chat is
    // meaningless solo) and only when no other surface owns input (typing 't'
    // into the browser's name field must not pop the chat). Swallow the press
    // so the game never acts on T and the input bar doesn't start with a "t".
    if (msg == WM_KEYDOWN && wParam == 'T' && !CaptureActive() && !PauseMenuOpen() &&
        coop::input::input_owner::MayTakeKey('T') && coop::chat_sync::SessionActive()) {
        ProbeKeyMsg(msg, wParam, "SWALLOWED by the T-chat hotkey");
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
        if (!CaptureActive() && coop::input::input_owner::MayTakeKey('V') &&
            coop::voice_chat::Enabled()) {
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
                    ProbeKeyMsg(msg, wParam, "SWALLOWED by CaptureActive");
                    return 1;
                default: break;
            }
        }
    }
    if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR ||
        msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
        ProbeKeyMsg(msg, wParam, "passed to the GAME");
    return ::CallWindowProcW(g_origWndProc, hwnd, msg, wParam, lParam);
}

// First-present bring-up. Returns true once ImGui + the RHI render backend are
// live. On ANY failure it releases whatever it acquired this call (no leak across
// retries). RHI-specific steps live behind ui/overlay_backend.h.
bool BringUp(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow) return false;

    if (!overlay_backend::CaptureDevice(sc)) return false;

    bool ctxCreated = false;
    if (!ImGui::GetCurrentContext()) { ImGui::CreateContext(); ctxCreated = true; }
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;     // don't litter a layout .ini next to the game
    io.MouseDrawCursor = true;    // ImGui draws its OWN cursor -- always visible (UE4 keeps
                                  // the OS cursor hidden during play); the WM_SETCURSOR
                                  // hide + SetCursorPos no-op keep it the only one + tracking
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // arrow-key move + Enter/Space activate
    // Resolution scale: seed from the game window's client size BEFORE the first
    // bake so fonts + style are born at the right size for this resolution
    // (ui/scale.h -- the overlay is proportional, not fixed-1080p-px).
    ui::scale::LoadUserPrefOnce();  // ini ui.scale (the F1 "UI size" pref)
    RECT rc{};
    if (::GetClientRect(desc.OutputWindow, &rc))
        ui::scale::NoteViewport(static_cast<float>(rc.right - rc.left),
                                static_cast<float>(rc.bottom - rc.top));
    ui::scale::ConsumeRebuild();  // the initial bake right here applies it
    // Reset-then-scale, exactly like ui::style::MaybeRescale: ScaleAllSizes is cumulative,
    // and a SEH-swallowed half-bring-up can re-enter here with an already-scaled
    // style on the leaked context (audit WARN-2) -- scaling again would compound.
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

    // FONTS AFTER THE RENDERER, and the order is an INVARIANT rather than a
    // necessity (2026-07-30, the ImGui 1.92 flip).
    //
    // ImGuiBackendFlags_RendererHasTextures is set INSIDE InitRenderer, and
    // ImFontAtlasBuildMain samples it off the context at the instant it runs --
    // so a Load() that reached a build while the flag was still clear would lock
    // in an EAGER atlas under a dynamic regime, which upstream names as a bug in
    // its own source. Load() no longer builds anything (both ImFontAtlas::Build()
    // calls retired in the same commit; the first build is now
    // ImFontAtlasUpdateNewFrame inside the first NewFrame), so today the hazard
    // is unreachable by a second route as well.
    //
    // It is ordered this way anyway, deliberately: "nothing in Load touches the
    // atlas" is a property of Load's current body, and the next glyph-touching
    // line added there would silently re-open the bug. This order makes it
    // impossible instead of merely absent. ui::style::MaybeRescale already exercises
    // Load-after-InitRenderer on every scale change, and both bring-up early
    // returns above are safe with or without Load having run
    // (fonts::OnContextDestroyed only nulls the role pointers).
    ui::fonts::Load();

    g_hwnd = desc.OutputWindow;
    g_origWndProc = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProcDetour)));
    UE_LOGI("imgui_overlay: %s bring-up OK (hwnd=%p) -- F1 toggles the menu",
            overlay_backend::Kind(), g_hwnd);
    return true;
}


// SEH-guarded per-frame ImGui pass (render thread). A fault here must NOT take down
// the game's render thread -- swallow it and leave the menu hidden.
void RenderFrameGuarded(IDXGISwapChain* sc) {
    __try {
        ui::style::MaybeRescale(g_hwnd);
        overlay_backend::NewFrame();
        ImGui_ImplWin32_NewFrame();  // sets io.MousePos from the real OS cursor (WM_MOUSEMOVE / GetCursorPos)
        // Draw the ImGui software cursor only for interactive surfaces (F1 menu, or
        // the host scoreboard). The passive client scoreboard shows no cursor.
        ImGui::GetIO().MouseDrawCursor = CaptureActive();
        ImGui::NewFrame();

        // The atlas is LAZY now, so nothing about it is settled at boot: it grows,
        // repacks and discards bakes while the game runs. This is the one call
        // site that watches it -- the superset invariant, the pack-failure
        // detector and the per-build selftest, each on its own trigger. It must
        // be INSIDE the frame (the selftest bakes one emoji on purpose, and an
        // out-of-frame atlas query is what poisoned TexIsBuilt in b132), and it
        // runs at the START so it measures what the previous frame's drawing and
        // this frame's UpdateNewFrame actually did.
        ui::atlas_watch::OnFrame();

        // Always-on PASSIVE coop HUD (screen-projected nameplates + the chat/event
        // feed). Drawn FIRST so the interactive surfaces below sit ON TOP of it. It
        // never captures input -- nameplates use the background draw list, the chat
        // window is NoInputs -- so CaptureActive() excludes it and the player keeps
        // playing while it overlays. SUPPRESSED while VOTV's native pause/ESC menu is up:
        // our ImGui renders after the game in Present, so the passive HUD would otherwise
        // draw on TOP of the modal pause menu (user 2026-06-13: chat over the ESC menu).
        if (ui::hud::IsActive() && !PauseMenuOpen()) ui::hud::Render();
        // The network-stats overlay (F1 > Network > Stats; off by default). Passive
        // like the HUD (NoInputs) and suppressed under the native pause menu the same
        // way; independent of hud::IsActive() (it must show for a solo host too).
        if (ui::net_stats_panel::Enabled() && !PauseMenuOpen()) ui::net_stats_panel::Render();

        if (MenuOpen())    ui::dev_menu::Render();
        if (ScoreOpen())   ui::scoreboard::Render();
        if (VoiceOpen())   ui::voice_panel::Render();
        if (BrowserOpen()) ui::server_browser::Render();
        // The connect-failed modal draws AFTER the browser so it layers on top of the
        // reopened browser (a failed browser join reopens it). No-ops when nothing pending.
        if (ui::connect_failed_dialog::IsOpen()) ui::connect_failed_dialog::Render();
        // The T10 config review (settings check): persistent-until-dismissed;
        // draws under the boot-warning modal (an install problem outranks a
        // settings report) and over the regular surfaces.
        if (ConfigReviewOpen()) ui::config_review_panel::Render();
        // The boot-warning modal (a boot-time install problem, e.g. the native
        // browser's missing-donor warning): layers over whatever surface is up
        // until acknowledged. No-ops when nothing pending.
        if (ui::boot_warning_dialog::IsOpen()) ui::boot_warning_dialog::Render();
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

        // Publish "our overlay is capturing typed text" for the global-key hotkey
        // pollers (voice PTT/whisper/mute, freecam, spawn-menu Q): while a text
        // field is focused, their GetAsyncKeyState reads must NOT fire, so a
        // keystroke meant for the field doesn't ALSO trigger a bind (2026-07-09:
        // T-to-chat then G activated voice). WantTextInput covers every text
        // widget; OR the chat-open latch for the one focus-handoff frame.
        const bool overlayText = ImGui::GetIO().WantTextInput || ui::chat_input::IsOpen();
        ui::input_focus::SetOverlayCapturingText(overlayText);
        coop::input::input_owner::PublishOverlayOwnsText(overlayText);

        // Cursor OWNERSHIP transition (MTA CLocalGUI::Draw shape) -- before the probe,
        // so the probe observes the state the frame will actually draw with.
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
        // Same re-fault guard for the connect-failed modal: clear the pending reason so a
        // faulted Render can't re-enter + re-fault every frame (the reason is its open flag).
        coop::join_progress::ClearFailReason();
        // And for the boot-warning modal (audit 2026-07-19: its pending text IS its open
        // flag; without this a faulted Render re-enters + re-faults forever).
        ui::boot_warning_dialog::Clear();
        // CLOSE the picker too: if its Render() faulted, leaving it open re-enters
        // Render() every frame and re-faults forever (audit HIGH-2 re-fault loop).
        ui::host_save_picker::Close();
        // Same re-fault guard for the loading screen (also resets join_progress so a
        // faulted join can't leave the connecting state stuck) and the console.
        ui::loading_screen::Close();
        ui::console::Close();
        // UNLATCH the text-capture publish (audit 2026-07-10): a fault above skips
        // the per-frame SetOverlayCapturingText publish -- with chat open at the
        // fault, the flag stays TRUE forever and every gated hotkey (voice PTT,
        // freecam, spawn-menu Q) goes permanently dead. Close the field and
        // publish keys-live, mirroring the frame's normal end-state.
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

// THE GUARD MUST COVER THE QueryInterface, NOT ONLY THE LOAD -- and the first version did
// not, while its own commit message said it did.
//
// `QueryInterface` is a VTABLE DISPATCH through the very pointer this exists to distrust: it
// reads `*(void***)sc` and calls through it. Guarding only the read of `viewport+off` guards
// the step that was never in doubt. The case the whole design is built to survive is a game
// recook that moves the swapchain field: the AOB still matches (the prologue is unchanged),
// `[viewport+0x70]` now holds a float or an FString*, it is NON-NULL so the null check
// passes, and the dispatch faults -- an access violation on the render thread, on the first
// frame, every launch, exactly instead of the one-line complaint this path exists to
// produce. (Post-ship audit, 2026-08-30.)
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

// THE SWAPCHAIN, RE-READ EVERY CALL AND QI-VALIDATED ON CHANGE (cross-cutting rule,
// docs/OVERLAY_CAPTURE_COEXIST.md section 9c). Two halves, and they answer different
// questions:
//
//   RE-READ EVERY CALL, because a fullscreen transition REPLACES the object. Caching the
//   pointer would keep drawing into a swapchain the engine has abandoned -- the shipped
//   DX12 backend already says so at overlay_backend_dx12.cpp:488 and re-compares every
//   call for the same reason.
//
//   QI ONLY ON CHANGE, because this runs on the present path at ~120 Hz and QueryInterface
//   is a COM call. What it proves is that our OFFSET is still the swapchain field, and an
//   offset does not drift between two frames of one process -- it drifts between BUILDS.
//   So the honest cadence is "prove each distinct pointer once", not "prove the same
//   pointer 120 times a second".
//
// FAILS CLOSED: an unprovable pointer yields null and the caller skips its work rather
// than handing D3D something we could not identify. The complaint is rate-limited to the
// pointer that caused it, so a permanently drifted offset costs one line, not one a frame.
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

// ONE FRAME OF OVERLAY WORK, and it no longer knows what called it.
//
// This body used to BE the `IDXGISwapChain::Present` detour. It is unchanged except that
// the swapchain now arrives as a parameter instead of being the hooked function's first
// argument -- the whole seam move is "who calls this and with what", not "what it does".
// docs/OVERLAY_CAPTURE_COEXIST.md section 9c commit 3.
void OverlayFrame(IDXGISwapChain* sc) {
    // Perf probe: count every presented frame (the frame anchor for ms/frame).
    // Cheap relaxed increment when armed; a single bool load when off.
    //
    // IT IS ALSO THE S1 INSTRUMENT, which is why it stays on this path. `fps=0` sustained
    // while every other subsystem ticks is how RTSS's unlink was measured, and it is the
    // ONLY reading that can say whether the seam move worked -- so the commit that takes
    // work off the present path must move this counter, never delete it.
    coop::dev::perf_probe::NoteFrame();
    // R-3 passive #2: time our WHOLE detour body (input publish + ImGui render)
    // up to -- but excluding -- the engine's own Present. Accumulated into the
    // OverlayPresent bucket (render thread; AddTicks is a relaxed fetch_add).
    // Cannot use perf_probe::Scope here: its destructor would fire AFTER
    // the engine's own present returns and swallow the present/vsync wait.
    const bool perfOn = coop::dev::perf_probe::Armed();
    const unsigned long long perfT0 = perfOn ? coop::dev::perf_probe::NowTicks() : 0;
    // The input probe must sample even with NO surface open (that is the state in which
    // the game owns text), so it hangs off Present, not off the ImGui pass below.
    coop::dev::input_focus_probe::NoteFrame();
    // Count frames presented per WorldKind (RUNG 0 of the native-UI probe). This is the
    // ONE place the question "does the game present frames when no world exists" can be
    // asked, and leaving the instrument out of the loop while banking an answer about it
    // is the blind-instrument shape docs/MULTIPLAYER_UI.md section 8 refuses.
    coop::dev::worldless_frames::NoteFrame();
    // Republish "does the game own typed text". Two cadences (see input_owner.h): the
    // cheap fast path at ~10 Hz, the GUObjectArray walk at ~1 Hz, both hopped onto the
    // game thread because they call engine UFunctions.
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
        // RULE 2, 2026-08-30: `g_inFrame` was two release-ordered atomic stores per rendered
        // frame maintaining a flag with ZERO readers -- its only consumer was a `Shutdown()`
        // deleted long ago, while its declaration still named that caller. Gone.
        overlay_backend::EnsureTarget(sc);  // recreate the render target after a resize
        RenderFrameGuarded(sc);
    }
    if (perfOn) {
        coop::dev::perf_probe::AddTicks(coop::dev::perf_probe::Bucket::OverlayPresent,
                                        coop::dev::perf_probe::NowTicks() - perfT0);
    }
}

// THE DRAW SEAM -- the whole fix for S1 and S2, in two functions.
//
// We used to inline-patch `IDXGISwapChain::Present`. That is the exact byte region RTSS
// runtime-disassembles and RESTORES: it lets us install, then unlinks us, and the overlay
// dies after a frame or two (measured `fps=0` sustained against a `120/s` control, and the
// user watching it "appear for a second and disappear"). It is also BELOW the point OBS's
// default game-capture copies the backbuffer, which is the same defect wearing the other
// symptom.
//
// So we no longer patch it. `FD3D11Viewport::PresentChecked` and
// `FD3D12Viewport::PresentInternal` are the engine's own private functions that CALL that
// swapchain Present -- census-proven single once-per-frame choke points. Drawing there puts
// our pixels in the backbuffer BEFORE the engine hands it to DXGI, i.e. upstream of the
// entire external inline-hook chain. RTSS has nothing of ours to unlink (it has no
// knowledge of a private engine function), its OSD still composites on top when the
// original runs, and OBS copies a backbuffer we are already in.
//
// WE DRAW UNCONDITIONALLY. `PresentChecked` has three non-presenting exits (the
// CustomPresent gate, a null swapchain, and a fullscreen-state mismatch). Pre-checking them
// would mean re-implementing the engine's conditions inside our detour -- a site list that
// breaks silently the day those conditions change. On a non-presenting exit our draw is
// wasted, not harmful.
//
// THE RETURN VALUE IS FORWARDED AS AN OPAQUE REGISTER-WIDTH INTEGER, deliberately. UE4.27
// declares both of these `bool`, but the DX12 one TAIL-JUMPS to IDXGISwapChain::Present
// (`48 FF 60 40`), whose return is an HRESULT -- so the two readings of "what is really in
// RAX" disagree, and this file does not need to settle it. Declaring the trampoline's
// return as an integer of register width forwards whatever the original produced,
// byte-exact, under either reading. Guessing `bool` and being wrong would hand the engine
// a truncated verdict about its own present.
using EnginePresentFn = uintptr_t(__fastcall*)(void* viewport, int32_t syncInterval);
EnginePresentFn g_d3d11PresentTrampoline = nullptr;
EnginePresentFn g_d3d12PresentTrampoline = nullptr;
// ONE proven-pointer cache per RHI, shared by that RHI's present seam and its resize
// bracket: they read the SAME field of the SAME viewport, so proving it twice proves
// nothing extra, and two caches could disagree about which pointer is current.
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

// THE RESIZE BRACKET, ON THE ENGINE'S OWN SEAM (docs/OVERLAY_CAPTURE_COEXIST.md section
// 9c commit 1). It replaces an inline hook on IDXGISwapChain::ResizeBuffers, and the
// replacement is not a refactor -- it fixes a live shipped crash.
//
// WHAT WENT WRONG WITH THE OLD SEAM. `IDXGISwapChain::ResizeBuffers` fails with
// DXGI_ERROR_INVALID_CALL unless every reference to the back buffers has been released
// first, and UE turns that failure into a Fatal (D3D11Viewport.cpp:298). Our render target
// view is such a reference, and the ONLY thing that released it was a detour on the very
// DXGI function RTSS unlinks. MEASURED 2026-08-23: with RTSS armed the first resize logged
// our bracket and the second logged NOTHING -- and killed the game.
//
// The engine's `FD3D11Viewport::Resize` is the caller of that ResizeBuffers, it is private
// to the game binary, and section 6c.b measured that nobody else patches it. So the bracket
// now runs whether or not a third-party overlay is in the process.
//
// THE SWAPCHAIN IS RE-READ AFTER THE ORIGINAL, NEVER CACHED. A fullscreen transition can
// replace the swapchain object entirely, so the pointer we release against and the pointer
// we re-create against are not assumed to be the same one. (Cross-cutting rule, section 9c;
// the shipped DX12 backend already states it at overlay_backend_dx12.cpp:488.)
//
// ATTRIBUTION IS STILL `[?]`. RTSS holds its own back-buffer references and its own
// ResizeBuffers hook was in the same unlinked state, so the outstanding reference may have
// been RTSS's rather than ours; the falsifier that would settle it (resize twice with RTSS
// armed BEFORE our bring-up) has not run. This bracket is correct either way -- releasing
// our own reference before a resize is right regardless of who else holds one -- but the
// arc may NOT claim the crash is cured until that runs.
// TWO SIGNATURES, BECAUSE THE TWO SEAMS ARE NOT THE SAME FUNCTION -- the first version
// declared one, which made the DX12 log line print numbers it could not know.
//
// DX11's `FD3D11Viewport::Resize(uint32,uint32,bool,EPixelFormat)` does take the four. The
// DX12 seam at image+0x177E8B0 is `FD3D12Viewport::ResizeInternal()`, which takes ONLY
// `this`: disassembled, its seventh instruction is `mov edx,3` -- it DESTROYS the register a
// second argument would arrive in -- and it reads the extent from a member
// (`mov edx,[rbx+0x90]`). Its own caller at image+0x1777110 is the real four-argument
// `Resize`, which stores those arguments to members and calls this with `mov rcx,rbp` and
// nothing else. (Post-ship audit, 2026-08-30, disassembled from the shipping PE.)
//
// The HOOKED FUNCTION is still the right one -- ResizeInternal IS the function that calls
// IDXGISwapChain::ResizeBuffers, so the bracket is if anything tighter than DX11's. Only its
// NAME and its ARITY were wrong.
using D3D11ResizeFn = void(__fastcall*)(void* viewport, uint32_t sizeX, uint32_t sizeY,
                                        uint8_t bFullscreen, int32_t pixelFormat);
using D3D12ResizeInternalFn = void(__fastcall*)(void* viewport);
D3D11ResizeFn         g_d3d11ResizeTrampoline = nullptr;
D3D12ResizeInternalFn g_d3d12ResizeTrampoline = nullptr;

// The half both seams share: re-derive the render target after the engine has resized. The
// CALL to the original stays with the caller, because the two originals do not take the same
// arguments -- and `sizeNote` is a pre-formatted string rather than four values for exactly
// that reason: only one of the two seams is given any.
void EngineResizeBracket(const char* which, size_t scOff, IDXGISwapChain*& lastOk,
                         void* viewport, const char* sizeNote) {
    IDXGISwapChain* sc = ValidatedSwapChain(viewport, scOff, which, lastOk);
    if (sc && g_imguiReady.load(std::memory_order_acquire)) {
        overlay_backend::OnResizeRecreate(sc);
        // Resizes are rare (a window/resolution/fullscreen change), so one line each is not
        // spam -- and without it a drill cannot tell whether this path ran at all
        // (2026-07-26: the DX12 resize drill was unmeasurable because only the FAILURE
        // branch logged). It is also the ONE line that says the bracket survived a run with
        // RTSS armed, which is the whole point of moving it.
        UE_LOGI("imgui_overlay: %s%s -- render target rebuilt on %s", which, sizeNote,
                overlay_backend::Kind() ? overlay_backend::Kind() : "?");
        ue_wrap::log::Flush();
    }
    // A null `sc` needs no line here: ValidatedSwapChain already complained, once, about
    // the pointer that caused it. The consequence -- no re-create, so the overlay draws
    // nothing until the next resize -- is stated there.
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
    // NO SIZE IN THE LINE, deliberately: ResizeInternal takes only `this` and reads the new
    // extent from a member, so four numbers here would be four numbers of register residue.
    EngineResizeBracket("FD3D12Viewport::ResizeInternal",
                        ue_wrap::profile::kD3D12Viewport_SwapChain, g_lastOkSc12, viewport, "");
}

// RULE 2, 2026-08-30: `ResolveSwapChainVtable` -- the throwaway DX11 device + hidden
// window + swapchain spun up ONLY to read the DXGI vtable -- is DELETED with the
// `IDXGISwapChain::Present` hook it existed to find. Nothing in this file patches a
// DXGI vtable entry any more; both seams are engine-private functions found by AOB.
//
// The trade is stated rather than hidden: that resolve was VERSION-IMMUNE (a vtable
// index is fixed by the DXGI contract) and the two AOBs are not. It is the price of
// not standing on a function RTSS restores, and it is the same price every other core
// capability in this mod already pays -- ProcessEvent rides an AOB too.

}  // namespace

bool Init() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    if (!ue_wrap::hook::Init()) { UE_LOGE("imgui_overlay: hook::Init failed"); return false; }
    // THE RESIZE BRACKET LIVES ON THE ENGINE'S OWN Resize, not on
    // IDXGISwapChain::ResizeBuffers (docs/OVERLAY_CAPTURE_COEXIST.md section 9c commit 1).
    // Both RHIs are installed unconditionally: which one the game presents with is not
    // known at Init time, the other signature simply will not resolve in a single-RHI
    // build, and a bracket that is only armed for the RHI we guessed is a bracket that is
    // missing on the other one -- where the crash it prevents is worse (the DX12 backend
    // AddRefs up to 8 back buffers behind the same single release).
    //
    // NOT FATAL IF A SIGNATURE IS STALE. Failing the whole overlay here would trade a
    // crash-on-resize for no UI at all on every recook; the honest degradation is to keep
    // drawing and say loudly that resizes are now unprotected.
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
                // Expected for the RHI this cook does not contain. What is NOT acceptable
                // is zero DRAW seams, which is checked after the loop.
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
        // FAIL CLOSED ON THE DRAW SEAM, AND ONLY ON IT. With no draw seam there is no
        // overlay at all -- no chat, no scoreboard, no F1 -- so this returns false and says
        // why, rather than installing a WndProc hook that swallows keys for a UI that can
        // never appear. A missing RESIZE bracket is a degradation, not an absence, and is
        // already reported above.
        if (drawArmed == 0) {
            UE_LOGE("imgui_overlay: NO DRAW SEAM. Neither FD3D11Viewport::PresentChecked nor "
                    "FD3D12Viewport::PresentInternal resolved, so the overlay cannot draw and "
                    "is DISABLED for this run. This is what a game recook looks like from "
                    "here -- re-derive the signatures per docs/VERSION_MIGRATION.md.");
            ue_wrap::log::Flush();
            return false;
        }
    }
    // Hook user32!SetCursorPos so we can neutralize UE4's per-tick cursor recenter
    // while the menu is visible (non-fatal if it can't hook -- the menu still works,
    // the cursor just won't track as cleanly).
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
    // DX12 stage-1: the swapchain-creation timing probe (log-only; measures
    // whether our boot precedes the game's swapchain creation -- see the DX12
    // overlay design of record). No behavior change on any RHI.
    ui::overlay_backend::InstallCreationProbe();
    g_installed.store(true, std::memory_order_release);
    // TEST-ONLY env arming (mp.py's entry seam into the UI surfaces + the
    // browser-path session scenarios) -- ui/overlay_test_arm.cpp. Inert unless a
    // VOTVCOOP_* test variable is set, so a normal player boot does nothing here.
    ui::overlay_test_arm::ArmFromEnv();
    UE_LOGI("imgui_overlay: draw seam installed on the ENGINE present path (%d of 4 "
            "engine seams armed) -- ImGui brings up on the first frame; press F1 "
            "in-game for the menu", g_seamsArmed);
    return true;
}

bool IsVisible() { return g_visible.load(std::memory_order_relaxed); }
void SetVisible(bool visible) { g_visible.store(visible, std::memory_order_relaxed); }
// Forced (not g_scoreboard) so it survives the host losing focus to the
// launching client window -- the WM_KILLFOCUS reset only clears the real tilde key.
void ForceScoreboardOpen() { g_scoreboardForced.store(true, std::memory_order_relaxed); }

std::string CaptureOwners() {
    // BY VALUE. The first version returned a pointer into a function-local static, which two
    // %s in one log line would alias and which no second thread could use safely -- a latent
    // trap in a function whose whole purpose is to be called from diagnostics written in a
    // hurry. Mirrors CaptureActive() term for term: if a term is ever added there and not
    // here, this stops naming the surface that is actually holding the mouse, which is the
    // one thing it exists to do.
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

// ---- process-exit retirement -------------------------------------------------
// RULE 2, 2026-08-26: a full `Shutdown()` lived here and `[V]` had ZERO callers
// tree-wide for its entire life -- as did every function it called, down to
// dx12_capture::Shutdown. It was also wrong twice: `hook::Uninstall` corrupts the
// trampoline in place, and its 200 ms g_inFrame wait is the in-flight counter
// hook.h:45-48 rejects, placed AFTER that free. A dying process needs one thing --
// stop new detour entries -- and hook::Shutdown's blanket disable does it for all
// three patches; `[V]` it has always been the only thing that ever lifted them.
// Do not re-add a teardown here. Full account: docs/UE4SS_ARC.md section 4c.

}  // namespace ui::imgui_overlay
