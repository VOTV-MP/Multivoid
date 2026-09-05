// ui/multiplayer_menu.cpp -- see ui/multiplayer_menu.h. Injects a MULTIPLAYER button above NEW
// GAME in the game's main menu and opens the server browser when it is clicked, with the same
// shape as the save-button disable: a POST observer on the menu's Tick (self is the menu),
// property offsets for the field reads, and the pause flag to target the main menu. The
// button itself is built by the engine wrapper's canvas inject; this file owns the feature:
// which menu, where, and what the click does. The menu tick also drives the native
// sub-screens and the version label.

#include "ui/multiplayer_menu.h"

#include "coop/config/config.h"
#include "ui/input_focus.h"
#include "coop/session/join_progress.h"
#include "coop/session/session_manager.h"  // LatestVersionLine, DisplayVersion
#include "ui/server_browser.h"
#include "ui/server_browser_surface.h"  // WHICH browser this session uses
#include "ui/native_screen.h"   // BeginMenuTick -- one index read per menu tick
#include "ui/browser_input_screens.h"
#include "ui/host_session_settings.h"
#include "ui/host_window_native.h"
#include "ui/server_browser_native.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/cached_obj_ref.h"
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

// Resolved once at install; the class, the function and the field offsets never move.
void* g_tickFn = nullptr;               // ui_menu_C::Tick (observer anchor)
int32_t g_buttonStartOff = -1;          // ui_menu_C -> button_start (UButton*, NEW GAME)
int32_t g_isPauseOff = -1;              // ui_menu_C -> isPause (bool)
int32_t g_txtVersionOff = -1;           // ui_menu_C -> txt_version (UTextBlock*, the version label)
int32_t g_switcherOff = -1;             // ui_menu_C -> switcher_widgets, the sub-screen layer the native screens join

// Injected-button tracking, game thread only: touched solely in the Tick observer.
void* g_injectedMenu = nullptr;         // the menu instance we last injected into (compared, never deref'd)
// Our MULTIPLAYER button, a cached reference rather than a raw pointer: the widget is freed
// with its menu instance for the whole play session and probed per menu tick on the return to
// the menu, and a bare liveness deref there is the first-chance fault a co-resident crash
// reporter pops as a crash.
ue_wrap::CachedObjRef g_button;
bool  g_buttonInputBlocked = false;     // edge-tracking: is g_button currently HitTestInvisible?
bool  g_prevLmb = false;                // VK_LBUTTON state last tick (click-edge detect)
bool  g_lmbPrimed = false;             // first-tick guard: seed g_prevLmb without firing an edge
uint64_t g_lastInjectMs = 0;            // throttle inject attempts on failure / self-heal
// The render-thread-readable pause-menu signal: stamped on the game thread on every pause-menu
// tick, reported open while the stamp is fresh, and auto-clearing about 250 ms after the
// pause menu stops ticking. Atomic, so the overlay reads it lock-free to keep the passive HUD
// off the native modal menu.
std::atomic<uint64_t> g_pauseTickMs{0};

// The native version label, game thread only: a text block injected as a sibling of the game's
// own version label, so the coop line sits among the game's build labels and shows and hides
// with the menu. Driven from the session manager's latest-version line.
ue_wrap::CachedObjRef g_versionText;    // our injected UTextBlock
void* g_versionMenu = nullptr;          // the menu instance we injected it into
// The label's normal colour, cyan, the coop accent matching the injected button; amber while an
// update is available.
constexpr ue_wrap::FLinearColor kVersionCyan{0.f, 1.f, 1.f, 1.f};
std::string g_versionLastLine;          // last string pushed to the block (edge-apply SetText)
bool g_versionLastOutdated = false;     // last colour state pushed (edge-apply SetColor)
uint64_t g_lastMainTickMs = 0;          // the main-menu tick timestamp
// The client loading state: the menu instance and the hidden state last applied for a join in
// progress. Edge-applied, so the visibility and opacity calls run only on a change. The menu
// pointer is compared, never dereferenced, so a destroyed menu is safe.
void* g_menuFadeMenu = nullptr;
bool  g_menuFadeHidden = false;

inline void* ReadPtr(void* base, int32_t off) {
    return (base && off >= 0) ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off)
                              : nullptr;
}

// Inject the button into the menu's NEW GAME list, above NEW GAME. Idempotent per instance;
// true if our button is present afterwards. Game thread only.
bool DoInject(void* menu) {
    if (menu == g_injectedMenu && g_button.Alive()) return true;  // already done
    void* buttonStart = ReadPtr(menu, g_buttonStartOff);
    if (!buttonStart) return false;  // menu not fully constructed yet
    g_button.Reset();
    void* btn = nullptr;
    if (E::InjectCanvasButton(buttonStart, L"Multiplayer", &btn)) {
        g_button.Set(btn);  // fresh from the inject -- the Set contract's shape
        g_injectedMenu = menu;
        UE_LOGI("multiplayer_menu: MULTIPLAYER button injected into menu=%p (button=%p)", menu, btn);
        return true;
    }
    return false;
}

// The current version line: the verdict if the check landed, else the plain identity, so the
// label is never empty. `outdated` selects the amber tint. Game thread.
std::string VersionLine(bool* outdated) {
    std::string line = coop::session_manager::LatestVersionLine(outdated);
    if (line.empty()) {
        if (outdated) *outdated = false;
        // The plain identity: the display version composite.
        line = coop::session_manager::DisplayVersion();
    }
    return line;
}

// Inject the version label once per menu instance as a sibling of the game's own, then keep
// its text and colour in sync with the verdict. Both are edge-applied, so a UFunction runs
// only on a real change. Game thread only.
void UpdateVersionLabel(void* menu) {
    if (g_txtVersionOff < 0) return;  // txt_version field not resolved -> label disabled
    // Once per menu instance, self-healing if the game rebuilt the menu widget.
    if (menu != g_versionMenu || !g_versionText.Alive()) {
        void* txtVersion = ReadPtr(menu, g_txtVersionOff);
        if (!txtVersion || !R::IsLive(txtVersion)) return;  // a fresh read off the live menu: bare IsLive is the contract
        g_versionText.Reset();
        bool outdated = false;
        const std::string initial = VersionLine(&outdated);
        const std::wstring winit(initial.begin(), initial.end());  // line is ASCII (URLs/idents)
        void* vt = nullptr;
        if (E::InjectTextRowAbove(txtVersion, winit.c_str(),
                                  &vt, /*outColor=*/nullptr)) {
            g_versionText.Set(vt);  // fresh from the inject
            g_versionMenu = menu;
            g_versionLastLine = initial;
            g_versionLastOutdated = outdated;
            // The block inherits the game label's colour from the style clone, so the coop accent
            // is applied (amber if we already know we are behind), and it must be the
            // colour-and-opacity dispatch: the block is already attached to Slate here, and a raw
            // property write would never repaint.
            const ue_wrap::FLinearColor amber{1.f, 0.78f, 0.35f, 1.f};
            E::SetTextBlockColorDispatch(vt, outdated ? amber : kVersionCyan);
            UE_LOGI("multiplayer_menu: native version label injected (text=%p) ABOVE txt_version=%p",
                    vt, txtVersion);
        }
        return;  // drive text/colour from next tick on
    }
    // Steady state: push text and colour only when the verdict changed. The raw read is legal
    // here, since the branch above validated the reference in the same tick.
    bool outdated = false;
    const std::string line = VersionLine(&outdated);
    if (line != g_versionLastLine) {
        const std::wstring wline(line.begin(), line.end());
        E::SetWidgetText(g_versionText.Raw(), wline.c_str());
        g_versionLastLine = line;
    }
    if (outdated != g_versionLastOutdated) {
        const ue_wrap::FLinearColor amber{1.f, 0.78f, 0.35f, 1.f};
        E::SetTextBlockColorDispatch(g_versionText.Raw(), outdated ? amber : kVersionCyan);
        g_versionLastOutdated = outdated;
    }
}

// The POST observer on the menu's Tick; self is the menu, with no scan. Game thread.
void OnMenuTickPost(void* self, void* /*function*/, void* /*params*/) {
    if (!self) return;
    // Main menu only: the pause menu shares the class but has no NEW GAME button to sit above.
    // While the pause menu is up, the freshness-stamped signal the render-thread HUD reads is
    // published, so the passive overlay is not drawn on top of the modal menu, and nothing below
    // applies.
    if (g_isPauseOff >= 0 && *(reinterpret_cast<uint8_t*>(self) + g_isPauseOff) != 0) {
        g_pauseTickMs.store(::GetTickCount64(), std::memory_order_relaxed);
        return;
    }
    // No update check runs on entering the title screen: that is not a request to talk to the
    // master, and it would give the master the player's address before any multiplayer decision.
    // The check rides the browser surface's open, which is such a request; the label shows the
    // local identity until a check has landed. Inject and drive the version label, a child of the
    // menu, so it shows and hides with it.
    UpdateVersionLabel(self);

    // The client loading state: while a join is in progress the whole menu widget is hidden, so
    // only the 3D background remains for the connecting screen to draw over, and restored when
    // the join completes or cancels. The hide is visual and functional: opacity 0 plus
    // hit-test-invisible, so the player cannot trigger options they cannot see, while the widget
    // keeps ticking so this observer can restore it. Edge-applied.
    {
        const bool hideForJoin = coop::join_progress::Active();
        if (self != g_menuFadeMenu || hideForJoin != g_menuFadeHidden) {
            // 3 is HitTestInvisible (self and children unclickable, still rendered); 0 is Visible.
            E::SetWidgetVisibility(self, hideForJoin ? 3 : 0);
            E::SetWidgetRenderOpacity(self, hideForJoin ? 0.0f : 1.0f);
            g_menuFadeMenu = self;
            g_menuFadeHidden = hideForJoin;
            UE_LOGI("multiplayer_menu: menu %s for connect (opacity %.0f, hit-test %s)",
                    hideForJoin ? "HIDDEN" : "restored", hideForJoin ? 0.0f : 1.0f,
                    hideForJoin ? "off" : "on");
        }
    }

    // Drive the native server browser from this observer rather than a second one on the same
    // UFunction: one owner of the menu tick. The switcher index is read once, before any screen:
    // all four compare the switcher's active index against their own, and asking the engine per
    // screen is four dispatches and four frame allocations per menu frame for one answer.
    ui::native_screen::BeginMenuTick(ReadPtr(self, g_switcherOff));
    ui::server_browser_native::OnMenuTick(self, ReadPtr(self, g_switcherOff));
    // The host window, its sibling in the same switcher; the same observer for the same reason.
    ui::host_window_native::OnMenuTick(self, ReadPtr(self, g_switcherOff));
    // The session settings, step two of hosting, immediately after step one; the order is
    // load-bearing: pressing Next raises this window's intent from inside the hosting window's
    // own click poll, and ticking step two next consumes it in the same tick, where the reverse
    // order would leave the player looking at the window they just left for a tick.
    ui::host_session_settings::OnMenuTick(self, ReadPtr(self, g_switcherOff));
    // The two small input windows (the address and the name typed in their own sub-windows, the
    // game's Language-window shape); the same observer. They build unconditionally and are only
    // shown by the browser's action grid.
    ui::browser_input_screens::OnMenuTick(self, ReadPtr(self, g_switcherOff));

    // Inject once per menu instance, self-healing if the game tore the button out, throttled to
    // one attempt a second so a persistent failure never hammers the spawn.
    const bool needInject = (self != g_injectedMenu) || !g_button.Alive();
    if (needInject) {
        const uint64_t now = ::GetTickCount64();
        if (now - g_lastInjectMs >= 1000) { g_lastInjectMs = now; DoInject(self); }
    }

    // While the server browser owns input, our button is made hit-test-invisible, so a click over
    // it cannot drive the native button's pressed visual: the overlay input hook swallows the
    // release while the browser is up, and the button would never see its mouse-up and stick
    // down. The native menu buttons are already blocked by that same swallow. Edge-applied, and
    // restored to visible the moment the browser closes.
    if (void* btn = g_button.Get()) {
        const bool block = ui::server_browser::IsOpen();
        if (block != g_buttonInputBlocked) {
            E::SetWidgetVisibility(btn, block ? 3 : 0);  // 3=HitTestInvisible, 0=Visible
            g_buttonInputBlocked = block;
        }
    }

    // The click poll opens the browser on the button's release edge while hovering our button.
    // Release, not press: the button is a real UButton whose mouse-down drives its pressed
    // visual, and opening on the down edge would flip capture on and the window hook would
    // swallow the release, leaving the button stuck down; on release it completes its own
    // press-and-spring first. IsHovered, a UFunction, is called only on the release edge.
    const bool down = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    // The edge state is seeded on the first tick, so a button already held when the observer
    // installs cannot synthesise a phantom click.
    if (!g_lmbPrimed) { g_lmbPrimed = true; g_prevLmb = down; }
    const bool releaseEdge = !down && g_prevLmb;
    g_prevLmb = down;
    void* clickBtn = releaseEdge ? g_button.Get() : nullptr;
    // Either browser counts as already open, and which one this click opens is not decided here:
    // the browser surface is the one owner of both questions, since the recovery paths in the
    // session runtime ask them too.
    if (clickBtn && !ui::server_browser_surface::IsOpen() &&
        !coop::join_progress::Active() &&  // suppress while connecting (the menu is hidden)
        ui::input_focus::IsOurWindowForeground() && E::WidgetIsHovered(clickBtn)) {
        UE_LOGI("multiplayer_menu: MULTIPLAYER clicked -> opening server browser");
        ui::server_browser_surface::Open();
    }
}

// Resolve the menu class and register the Tick observer; true once installed. Idempotent; game
// thread.
bool TryInstall() {
    if (g_installed.load(std::memory_order_acquire)) return true;

    void* uiMenuCls = R::FindClass(prof::name::UiMenuClass);
    if (!uiMenuCls) return false;  // menu BP not loaded yet -- caller retries

    g_tickFn         = R::FindFunction(uiMenuCls, prof::name::UiMenuTickFn);
    g_buttonStartOff = R::FindPropertyOffset(uiMenuCls, prof::name::UiMenuButtonStartProp);
    g_isPauseOff     = R::FindPropertyOffset(uiMenuCls, prof::name::UiMenuIsPauseProp);
    // The game's version label is the anchor for ours; non-fatal if absent, the label just does
    // not inject.
    g_txtVersionOff  = R::FindPropertyOffset(uiMenuCls, prof::name::UiMenuTxtVersionProp);
    // The switcher: non-fatal if absent, only the native screens need it.
    g_switcherOff    = R::FindPropertyOffset(uiMenuCls, L"switcher_widgets");
    if (g_switcherOff < 0)
        UE_LOGW("multiplayer_menu: switcher_widgets offset unresolved -- the native browser "
                "cannot be built this session");
    if (g_txtVersionOff < 0)
        UE_LOGW("multiplayer_menu: txt_version offset unresolved -- native version label disabled");
    // The start button is the only field the inject needs (its vertical box and its slot layout
    // and button style are derived from it); the pause flag gates main versus pause. The label
    // font and colour are set deterministically in the canvas inject.
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

// A bounded retry: the menu class may not be loaded the instant Init runs at boot, so the
// install is posted to the game thread every 500 ms until it succeeds or about a minute
// passes. One thread, self-exiting on success.
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
    // The opt-out kill switch: on by default, since this is a shipping feature and not gated by
    // the dev master switch.
    if (coop::config::ResolveFlag(::coop::config_registry::rows::multiplayer_menu_off)) {
        UE_LOGI("multiplayer_menu: disabled via [coop] multiplayer_menu_off=1");
        return;
    }
    // Try immediately (the menu is usually already up at boot), else retry.
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
    // The pause menu's Tick fires every frame while it is up, so a stamp within the last 250 ms
    // means open; once it closes the stamping stops and this falls back to false. Lock-free, so it
    // is safe from the render thread and the window-procedure thread.
    const uint64_t t = g_pauseTickMs.load(std::memory_order_relaxed);
    return t != 0 && (::GetTickCount64() - t) < 250;
}

void* MenuTickFn() {
    // Resolved once at install, at the boot menu, and never moves; null only if the menu class
    // never resolved, in which case the death-flee bypass falls back to its time ceiling.
    return g_tickFn;
}

void ForceInjectNow() {
    // The test hook for the menu-proceed scenario: inject deterministically on the live menu,
    // bypassing the observer-timing race in the brief post-bypass screenshot window. Ignores the
    // pause flag, since the caller has already reached the main menu. Game thread only.
    if (!g_installed.load(std::memory_order_acquire)) TryInstall();
    void* menu = R::FindObjectByClass(prof::name::UiMenuClass);
    if (!menu || !R::IsLive(menu)) { UE_LOGW("multiplayer_menu: ForceInjectNow -- no live ui_menu_C"); return; }
    UE_LOGW("multiplayer_menu: ForceInjectNow on menu=%p -> %s", menu, DoInject(menu) ? "injected" : "FAILED");
}

}  // namespace coop::multiplayer_menu
