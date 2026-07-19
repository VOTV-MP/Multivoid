// coop/multiplayer_menu.cpp -- see coop/multiplayer_menu.h.
//
// Injects a "MULTIPLAYER" UButton above NEW GAME in VOTV's main menu and opens the
// ImGui server browser when clicked. Mirrors coop::save_button_disable: a POST
// observer on ui_menu_C::Tick (self IS the menu), FindPropertyOffset for the field
// reads, isPause to target the MAIN menu (not the pause menu). The button is built
// by ue_wrap::engine::InjectCanvasButton (engine substrate); this file owns the
// feature: which menu, where, and that the click opens the browser.

#include "ui/multiplayer_menu.h"

#include "coop/config/config.h"
#include "ui/input_focus.h"
#include "coop/session/join_progress.h"
#include "coop/session/session_manager.h"  // RefreshLatestVersion + LatestVersionLine (native version label)
#include "ui/server_browser.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace coop::multiplayer_menu {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;
namespace prof = ue_wrap::profile;

namespace {

std::atomic<bool> g_installed{false};   // observer registered
std::atomic<bool> g_retrying{false};    // a retry thread is already running

// Resolved once at install (UClass + UFunction + field offsets never move).
void* g_tickFn = nullptr;               // ui_menu_C::Tick (observer anchor)
int32_t g_buttonStartOff = -1;          // ui_menu_C -> button_start (UButton*, NEW GAME)
int32_t g_isPauseOff = -1;              // ui_menu_C -> isPause (bool)
int32_t g_txtVersionOff = -1;           // ui_menu_C -> txt_version (UTextBlock*, the version label)

// Injected-button tracking (game-thread only -- touched solely in the Tick observer).
void* g_injectedMenu = nullptr;         // the menu instance we last injected into
void* g_button = nullptr;               // our MULTIPLAYER UButton
bool  g_buttonInputBlocked = false;     // edge-tracking: is g_button currently HitTestInvisible?
bool  g_prevLmb = false;                // VK_LBUTTON state last tick (click-edge detect)
bool  g_lmbPrimed = false;             // first-tick guard: seed g_prevLmb without firing an edge
uint64_t g_lastInjectMs = 0;            // throttle inject attempts on failure / self-heal
// Render-thread-readable "pause/ESC menu is up" signal. Stamped (game thread) on every
// pause-menu tick in OnMenuTickPost; IsPauseMenuOpen() reports open while the stamp is
// fresh and auto-clears ~250 ms after the pause menu stops ticking (closed / back to
// gameplay). std::atomic so the ImGui overlay (render thread) reads it lock-free -- it
// gates the passive coop HUD (chat feed / nameplates) off so we never draw OVER the
// native modal pause menu.
std::atomic<uint64_t> g_pauseTickMs{0};

// Native version/update label (game-thread only -- touched solely in the Tick observer).
// A UTextBlock we inject as a sibling of VOTV's txt_version ("Alpha 0.9.0 / Build a090n"),
// so the coop line sits organically among the game's build labels and auto show/hides with
// the menu. Driven from session_manager::LatestVersionLine; refreshed on each menu entrance.
void* g_versionText = nullptr;          // our injected UTextBlock
void* g_versionMenu = nullptr;          // the menu instance we injected it into
// "Normal" label colour: CYAN -- the coop accent, matching the injected MULTIPLAYER
// button (user 2026-07-16). Amber overrides while an update is available.
constexpr ue_wrap::FLinearColor kVersionCyan{0.f, 1.f, 1.f, 1.f};
std::string g_versionLastLine;          // last string pushed to the block (edge-apply SetText)
bool g_versionLastOutdated = false;     // last colour state pushed (edge-apply SetColor)
uint64_t g_lastMainTickMs = 0;          // main-menu tick timestamp; a >500ms gap = a fresh ENTRANCE (re-poll edge)
// Client loading state: the menu instance + hidden-state we last applied for a join-in-
// progress fade. Edge-applied so the SetVisibility/SetRenderOpacity UFunctions run only on
// a change, not per tick. (g_menuFadeMenu is never dereferenced -- pointer compare only --
// so a destroyed menu is safe.)
void* g_menuFadeMenu = nullptr;
bool  g_menuFadeHidden = false;

inline void* ReadPtr(void* base, int32_t off) {
    return (base && off >= 0) ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off)
                              : nullptr;
}

// Inject the MULTIPLAYER button into `menu`'s NEW GAME list (above NEW GAME).
// Idempotent per instance (no-op if our button is already live in this menu).
// Returns true if our button is present afterward. Game thread only.
bool DoInject(void* menu) {
    if (menu == g_injectedMenu && g_button && R::IsLive(g_button)) return true;  // already done
    void* buttonStart = ReadPtr(menu, g_buttonStartOff);
    if (!buttonStart) return false;  // menu not fully constructed yet
    g_button = nullptr;
    if (E::InjectCanvasButton(buttonStart, L"Multiplayer", &g_button)) {
        g_injectedMenu = menu;
        UE_LOGI("multiplayer_menu: MULTIPLAYER button injected into menu=%p (button=%p)", menu, g_button);
        return true;
    }
    return false;
}

// The current version/update line (verdict if the check landed, else a plain identity so
// the label is never empty). `outdated` => amber tint. Game thread.
std::string VersionLine(bool* outdated) {
    std::string line = coop::session_manager::LatestVersionLine(outdated);
    if (line.empty()) {
        if (outdated) *outdated = false;
        // Plain identity (no update verdict yet / none available): the v122
        // Paper-shape composite "votv-coop 0.0.1 b122 (VOTV 0.9.0-n)".
        line = coop::session_manager::DisplayVersion();
    }
    return line;
}

// Inject (once per menu instance) our NATIVE version/update label as a sibling of VOTV's
// txt_version, then keep its text + colour in sync with the /v1/latest verdict. Text and
// colour are edge-applied (a UFunction runs only on a real change), so steady state costs
// nothing. Game thread only.
void UpdateVersionLabel(void* menu) {
    if (g_txtVersionOff < 0) return;  // txt_version field not resolved -> label disabled
    // Inject once per menu instance; self-heal if VOTV ever rebuilt the menu widget.
    if (menu != g_versionMenu || !g_versionText || !R::IsLive(g_versionText)) {
        void* txtVersion = ReadPtr(menu, g_txtVersionOff);
        if (!txtVersion || !R::IsLive(txtVersion)) return;  // game's label not constructed yet
        g_versionText = nullptr;
        bool outdated = false;
        const std::string initial = VersionLine(&outdated);
        const std::wstring winit(initial.begin(), initial.end());  // line is ASCII (URLs/idents)
        if (E::InjectTextRowAbove(txtVersion, winit.c_str(),
                                  &g_versionText, /*outColor=*/nullptr)) {
            g_versionMenu = menu;
            g_versionLastLine = initial;
            g_versionLastOutdated = outdated;
            // The block inherits txt_version's colour from the style clone -- override
            // with the coop accent (cyan; amber if we already know we're behind). MUST be
            // the SetColorAndOpacity DISPATCH: the block is already attached to Slate at
            // this point, so a raw property write would never repaint (user 2026-07-16:
            // "no cyan" -- the raw write was exactly this trap).
            const ue_wrap::FLinearColor amber{1.f, 0.78f, 0.35f, 1.f};
            E::SetTextBlockColorDispatch(g_versionText, outdated ? amber : kVersionCyan);
            UE_LOGI("multiplayer_menu: native version label injected (text=%p) ABOVE txt_version=%p",
                    g_versionText, txtVersion);
        }
        return;  // drive text/colour from next tick on
    }
    // Steady state: push text/colour only when the verdict changed.
    bool outdated = false;
    const std::string line = VersionLine(&outdated);
    if (line != g_versionLastLine) {
        const std::wstring wline(line.begin(), line.end());
        E::SetWidgetText(g_versionText, wline.c_str());
        g_versionLastLine = line;
    }
    if (outdated != g_versionLastOutdated) {
        const ue_wrap::FLinearColor amber{1.f, 0.78f, 0.35f, 1.f};
        E::SetTextBlockColorDispatch(g_versionText, outdated ? amber : kVersionCyan);
        g_versionLastOutdated = outdated;
    }
}

// POST observer on ui_menu_C::Tick. `self` IS the menu (zero scan). Game thread.
void OnMenuTickPost(void* self, void* /*function*/, void* /*params*/) {
    if (!self) return;
    // MAIN menu only -- the pause menu (isPause==true) shares ui_menu_C but has no
    // NEW GAME button to sit above. While the pause menu IS up, publish the freshness-
    // stamped signal the render-thread HUD reads (IsPauseMenuOpen) so the passive coop
    // overlay (chat feed / nameplates) is not drawn on top of the modal pause menu, then
    // bail -- none of the main-menu inject/fade logic below applies to the pause menu.
    if (g_isPauseOff >= 0 && *(reinterpret_cast<uint8_t*>(self) + g_isPauseOff) != 0) {
        g_pauseTickMs.store(::GetTickCount64(), std::memory_order_relaxed);
        return;
    }
    // Reached only on the MAIN menu (isPause==false). Detect a fresh ENTRANCE: the menu
    // stops ticking during gameplay, so a >500 ms gap since the last main-menu tick means
    // the player just (re)entered the title screen -> re-poll /v1/latest so the version
    // label reflects the newest release. RefreshLatestVersion is self-debounced (one
    // fetch in flight + an 8 s floor between starts), so a stutter or rapid re-entry can't
    // DoS the master. Fires on first boot too (g_lastMainTickMs == 0).
    {
        const uint64_t now = ::GetTickCount64();
        if (g_lastMainTickMs == 0 || now - g_lastMainTickMs > 500)
            coop::session_manager::RefreshLatestVersion();
        g_lastMainTickMs = now;
    }
    // Inject / drive the native version label (sibling of txt_version). Auto show/hides
    // with the menu (it's a child), so no viewport add/remove or visibility gating.
    UpdateVersionLabel(self);

    // Client loading state: while a join is in progress, hide the WHOLE menu widget so only
    // the 3D menu background remains -- the "clean menu canvas" the connecting screen
    // (ui/loading_screen) draws its centered progress over -- then restore it when the join
    // completes/cancels. The hide is BOTH visual AND functional: opacity 0 (invisible) +
    // HitTestInvisible (the widget and all its children stop receiving clicks, so the user
    // can't trigger menu options they can't see). HitTestInvisible keeps the menu rendered/
    // ticking, so this same observer restores it. Edge-applied (no per-tick UFunction).
    {
        const bool hideForJoin = coop::join_progress::Active();
        if (self != g_menuFadeMenu || hideForJoin != g_menuFadeHidden) {
            // 3 = HitTestInvisible (self + children non-clickable, still rendered); 0 = Visible.
            E::SetWidgetVisibility(self, hideForJoin ? 3 : 0);
            E::SetWidgetRenderOpacity(self, hideForJoin ? 0.0f : 1.0f);
            g_menuFadeMenu = self;
            g_menuFadeHidden = hideForJoin;
            UE_LOGI("multiplayer_menu: menu %s for connect (opacity %.0f, hit-test %s)",
                    hideForJoin ? "HIDDEN" : "restored", hideForJoin ? 0.0f : 1.0f,
                    hideForJoin ? "off" : "on");
        }
    }

    // Inject once per menu instance; self-heal if VOTV ever tore our button out
    // (throttled to 1 attempt/s so a persistent failure never hammers SpawnObject).
    const bool needInject = (self != g_injectedMenu) || !g_button || !R::IsLive(g_button);
    if (needInject) {
        const uint64_t now = ::GetTickCount64();
        if (now - g_lastInjectMs >= 1000) { g_lastInjectMs = now; DoInject(self); }
    }

    // While the server browser owns input, make OUR button NON-interactive (HitTest
    // invisible) so a click over it cannot drive the native UButton pressed visual. If
    // it could, the overlay input hook (imgui_overlay) swallows the release while the
    // browser is up -> the button never sees its mouse-up and sticks DOWN until the next
    // click. The native menu buttons are already blocked by that same input swallow;
    // this makes ours behave identically. Edge-applied (SetVisibility only on a change);
    // restored to Visible (input-receiving) the moment the browser closes.
    if (g_button && R::IsLive(g_button)) {
        const bool block = ui::server_browser::IsOpen();
        if (block != g_buttonInputBlocked) {
            E::SetWidgetVisibility(g_button, block ? 3 : 0);  // 3=HitTestInvisible, 0=Visible
            g_buttonInputBlocked = block;
        }
    }

    // Click poll: open the browser on the LBUTTON RELEASE edge (not the press edge)
    // while hovering our button. Releasing -- not pressing -- is deliberate: our button
    // is a real UButton, so the mouse-DOWN drives its native Pressed (moved-down) visual.
    // If we opened the browser on the down edge, CaptureActive() flips true and the
    // WndProc hook (imgui_overlay) then SWALLOWS the WM_LBUTTONUP -> the UButton never
    // sees its release and stays stuck DOWN. Triggering on release lets the button
    // complete its own press->release ("moves down, springs back") exactly like the
    // native items; we open only after that. IsHovered() (a UFunction) is called ONLY on
    // the release edge, not per frame. Suppressed while the browser is already up.
    const bool down = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    // Seed the edge state on the very first tick so a mouse button already held
    // when the observer installs can't synthesize a phantom click on the button.
    if (!g_lmbPrimed) { g_lmbPrimed = true; g_prevLmb = down; }
    const bool releaseEdge = !down && g_prevLmb;
    g_prevLmb = down;
    if (releaseEdge && g_button && R::IsLive(g_button) && !ui::server_browser::IsOpen() &&
        !coop::join_progress::Active() &&  // suppress while connecting (the menu is hidden)
        ui::input_focus::IsOurWindowForeground() && E::WidgetIsHovered(g_button)) {
        UE_LOGI("multiplayer_menu: MULTIPLAYER clicked -> opening server browser");
        ui::server_browser::Open();
    }
}

// Resolve ui_menu_C + register the Tick observer. Returns true once installed.
// Idempotent. Runs on the game thread (reflection + observer registration).
bool TryInstall() {
    if (g_installed.load(std::memory_order_acquire)) return true;

    void* uiMenuCls = R::FindClass(prof::name::UiMenuClass);
    if (!uiMenuCls) return false;  // menu BP not loaded yet -- caller retries

    g_tickFn         = R::FindFunction(uiMenuCls, prof::name::UiMenuTickFn);
    g_buttonStartOff = R::FindPropertyOffset(uiMenuCls, prof::name::UiMenuButtonStartProp);
    g_isPauseOff     = R::FindPropertyOffset(uiMenuCls, prof::name::UiMenuIsPauseProp);
    // txt_version is the anchor for the native coop version label (non-fatal if absent --
    // the label just won't inject; the button + fade still work).
    g_txtVersionOff  = R::FindPropertyOffset(uiMenuCls, prof::name::UiMenuTxtVersionProp);
    if (g_txtVersionOff < 0)
        UE_LOGW("multiplayer_menu: txt_version offset unresolved -- native version label disabled");
    // button_start is the only field the inject NEEDS (we derive its VerticalBox +
    // clone its slot layout / button style); isPause gates main-vs-pause. The label
    // font/colour is set deterministically in InjectCanvasButton (font_ui + cyan), so
    // no tex_btnStart clone-source is needed.
    if (!g_tickFn || g_buttonStartOff < 0 || g_isPauseOff < 0) {
        UE_LOGW("multiplayer_menu: resolve incomplete (tick=%p button_start=%d isPause=%d) -- retry",
                g_tickFn, g_buttonStartOff, g_isPauseOff);
        return false;
    }
    if (!GT::RegisterPostObserver(g_tickFn, &OnMenuTickPost)) {
        UE_LOGE("multiplayer_menu: RegisterPostObserver(Tick) failed -- observer table full?");
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("multiplayer_menu: INSTALLED (tickFn=%p button_start@+0x%X isPause@+0x%X txt_version@+0x%X)",
            g_tickFn, static_cast<unsigned>(g_buttonStartOff),
            static_cast<unsigned>(g_isPauseOff), static_cast<unsigned>(g_txtVersionOff));
    return true;
}

// Bounded retry: ui_menu_C may not be loaded the instant Init() runs at boot. Post
// TryInstall to the game thread every 500 ms until it succeeds (or ~60 s elapses).
// One thread, self-exits on success. Mirrors freecam's lazy driver-thread pattern.
DWORD WINAPI RetryThread(LPVOID) {
    for (int i = 0; i < 120 && !g_installed.load(std::memory_order_acquire); ++i) {
        GT::Post([] { TryInstall(); });
        ::Sleep(500);
    }
    g_retrying.store(false, std::memory_order_release);
    return 0;
}

}  // namespace

void Init() {
    // Opt-out kill switch (default ON -- this is a shipping feature, not a dev one,
    // so it is NOT gated by the [dev] master switch). `[coop] multiplayer_menu_off=1`
    // disables it.
    if (coop::config::IsIniKeyTrue("multiplayer_menu_off")) {
        UE_LOGI("multiplayer_menu: disabled via [coop] multiplayer_menu_off=1");
        return;
    }
    // Try immediately (the menu is usually already up when we boot); else retry.
    GT::Post([] {
        if (!TryInstall() && !g_retrying.exchange(true)) {
            if (HANDLE t = ::CreateThread(nullptr, 0, &RetryThread, nullptr, 0, nullptr))
                ::CloseHandle(t);
            else
                g_retrying.store(false, std::memory_order_release);
        }
    });
}

bool IsPauseMenuOpen() {
    // The pause-menu Tick fires ~every frame while it's up, so a stamp within the last
    // ~250 ms means it is currently open; once it closes, stamping stops and this falls
    // back to false within the window. Lock-free (atomic load + GetTickCount64) so it is
    // safe to call from the render thread (the ImGui overlay) and the WndProc thread.
    const uint64_t t = g_pauseTickMs.load(std::memory_order_relaxed);
    return t != 0 && (::GetTickCount64() - t) < 250;
}

void* MenuTickFn() {
    // g_tickFn is resolved once at install (at the boot menu, well before any
    // gameplay death) and never moves -- UFunctions don't unload. Null only if
    // the menu class never resolved, in which case the death-flee bypass falls
    // back to its time ceiling.
    return g_tickFn;
}

void ForceInjectNow() {
    // TEST hook (coop::dev::menu_proceed): inject deterministically on the live menu,
    // bypassing the observer-timing race in the brief post-bypass screenshot window.
    // Ignores isPause (the caller has already reached the main menu). Game thread only.
    if (!g_installed.load(std::memory_order_acquire)) TryInstall();
    void* menu = R::FindObjectByClass(prof::name::UiMenuClass);
    if (!menu || !R::IsLive(menu)) { UE_LOGW("multiplayer_menu: ForceInjectNow -- no live ui_menu_C"); return; }
    UE_LOGW("multiplayer_menu: ForceInjectNow on menu=%p -> %s", menu, DoInject(menu) ? "injected" : "FAILED");
}

}  // namespace coop::multiplayer_menu
