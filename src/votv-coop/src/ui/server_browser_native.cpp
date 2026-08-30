// ui/server_browser_native.cpp -- see ui/server_browser_native.h.

#include "ui/server_browser_native.h"

#include "coop/config/config.h"
#include "coop/session/session_manager.h"
#include "coop/text/utf8_codec.h"       // the ONE owner of text encoding -- the footer
                                        // carries status text and click answers, both of
                                        // which can name a server
#include "ui/boot_warning_dialog.h"     // the loud failure surface for a donor that never appears
#include "ui/input_focus.h"            // a click only counts while OUR window is foreground
#include "ui/native_screen.h"          // palette + widget primitives, shared with the host window
#include "ui/server_browser_actions.h"   // CONNECT / HOST / REFRESH, its own TU
#include "ui/server_browser_rows.h"      // the LIST -- rows, identity, hover, selection
#include "ui/server_browser_selftest.h"  // the dev phase machine; ships dark
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <atomic>
#include <string>

namespace ui::server_browser_native {
namespace {

namespace E  = ue_wrap::engine;
namespace U  = ue_wrap::umg;
namespace P  = ue_wrap::profile;
namespace sm = coop::session_manager;
namespace selftest = ui::server_browser_selftest;
namespace rows = ui::server_browser_rows;

using ue_wrap::FLinearColor;

// ---- layout constants ------------------------------------------------------------
// Slate units. The WINDOW's own; the row metrics live with the rows.
constexpr float kWindowW  = 980.f;
constexpr float kWindowH  = 620.f;
// Frame + spacing, from the native windows (style doc section 3).
constexpr float kBorderPx = 2.f;
constexpr float kPadPx    = 6.f;
// The list's height is EXPLICIT, not the VerticalBox's leftover slack.
//
// MEASURED 2026-08-26: with a Fill slot the box allotted the ScrollBox 542 px inside a
// window that only had ~484 px left for it (offsetOfEnd 1438 against 30 rows of 66 px puts
// the viewport at 542), so the list overflowed UPWARD and its first row was drawn clipped
// under the column header. Slack arithmetic depends on every sibling's desired size being
// what you assumed; an override depends on nothing.
constexpr float kListH    = 470.f;
// ---- the construction kit ----------------------------------------------------------
// Alignment enums, the palette, and the widget primitives moved to ui/native_screen
// (2026-08-29) when the hosting window became the second native screen. The names are
// re-bound here so every call site below reads exactly as it did -- the extraction is a
// MOVE, and a move that rewrites its call sites cannot be diffed against the original.
namespace NS = ui::native_screen;
using NS::kFill;
using NS::kCenter;
using NS::kJustLeft;
using NS::kJustCenter;
using NS::SwitcherChild;
using NS::DonorField;
using NS::Spawn;
using NS::AddText;
using NS::AddFramedBox;
using NS::BuildButton;

// Only the WINDOW's colours are here; the row palette moved with the rows.
const FLinearColor kPanel  = NS::Panel();   // window fill
const FLinearColor kBorder = NS::Border();  // every frame in the game's menus
const FLinearColor kText   = NS::Text();    // the default: most text is white

// ---- state (GAME THREAD ONLY unless marked) --------------------------------------
void* g_menu     = nullptr;   // the ui_menu_C we built into (compared, never dereferenced)
void* g_switcher = nullptr;
void* g_root     = nullptr;   // our UUserWidget
void* g_status   = nullptr;   // the footer UTextBlock
void* g_scrimW   = nullptr;   // the full-screen scrim -- the thing that absorbs a stray click
void* g_closeBtn = nullptr;   // the X, top-right of the title row
void* g_backBtn  = nullptr;   // BACK, bottom-right beside the status line
// LBUTTON edge state for the chrome poll. Primed on Show() so the very release that
// OPENED the screen cannot be read as a click on the X sitting under the cursor.
bool  g_prevLmb   = false;
bool  g_lmbPrimed = false;

// ESC edge state. Primed on the first tick a screen is shown so a key already held when it
// opens cannot synthesize a close -- the same guard multiplayer_menu's click poll uses.
bool  g_prevEsc   = false;
bool  g_escPrimed = false;
int32_t g_ourIndex   = -1;
int32_t g_priorIndex = -1;
bool    g_shown      = false;

uint64_t g_lastRefreshMs = 0;
// A click's answer holds the footer for this long against the list sync's rewrite.
constexpr uint64_t kNoticeMs = 6000;
uint64_t g_noticeUntilMs = 0;
int      g_buildAttempts = 0;
bool     g_toldTheUser   = false;

// Cross-thread: the deferred open intent and the pointer-moved flag.
std::atomic<uint64_t> g_wantOpenMs{0};   // 0 = no intent
std::atomic<bool>     g_wantClose{false};
constexpr uint64_t kIntentTtlMs  = 20000;  // a join that never returns to a menu must expire
// FIVE SECONDS, WHICH IS WHAT WAS DECIDED. This read 1000 while
// docs/MULTIPLAYER_UI.md section 8c.-1 recorded "USER 2026-08-26: the re-fetch cadence is
// 5 s" -- and that section's own arithmetic is computed against 5 s ("one stalled frame
// per sync is 0.17% of frames at the 5 s cadence"), so at 1 Hz every cost number in the
// perf lane was understated 5x. It also gates `sm::Refresh()`, which spawns a detached
// thread and a TLS handshake per fetch, so the old value paid five of those per five
// seconds for a list that changes on the scale of a person deciding to host.
constexpr uint64_t kRefreshMs    = 5000;

// The footer mirrors the session's own status EXCEPT while a click's answer is still
// fresh. Without this the list sync would wipe "Pick a server from the list first."
// within a few frames of the player reading it, which reads as the button doing nothing.
//
// It sits BESIDE the list sync rather than inside it (2026-08-30, the row extraction): the
// footer is window chrome and the rows are the list, and the only reason they were one
// function is that both were wanted at the same two moments.
void RefreshStatusLine() {
    if (g_status && ::GetTickCount64() >= g_noticeUntilMs) {
        const std::string s = sm::Status();
        const std::wstring w = coop::text::FromUtf8Lossy(s.data(), s.size());
        E::SetWidgetText(g_status, w.c_str());
    }
}

// Pull the list and then the footer, in that order -- which is the order the one function
// they used to share performed them in.
void SyncRows() {
    rows::Sync();
    RefreshStatusLine();
}

// Build the screen once per menu instance. FAIL-CLOSED: a null donor means DO NOT BUILD
// and retry, never fall back to a default style -- that fallback is the Roboto/centred/
// white bug. After enough attempts the user is TOLD, because a silent forever-retry is the
// same defect one level quieter.
bool BuildScreen(void* switcher) {
    // BACKED OFF ONCE THE USER HAS BEEN TOLD, because the retry is not free and the thing
    // it waits for is not coming.
    //
    // Each failed attempt costs two `SwitcherChild` walks (a ChildCount plus a ClassNameOf
    // per child -- an engine alloc and a wstring EACH) and three `DonorField` lookups that
    // render a name per property while climbing SuperStruct. At ~117 menu ticks a second
    // that is tens of thousands of engine allocations per second, forever, on the exact
    // path a version migration lands on -- and the boot dialog is already up telling the
    // player the screen could not be built. Once we have said so, once a second is plenty.
    // (Post-ship perf audit 2026-08-30, F4. It matters more since the native browser became
    // the default: this now runs for every player, not only behind a dev flag.)
    if (g_toldTheUser) {
        static uint64_t sNextTryMs = 0;
        const uint64_t now = ::GetTickCount64();
        if (now < sNextTryMs) return false;
        sNextTryMs = now + 1000;
    }
    void* saveSlots = SwitcherChild(switcher, L"ui_saveSlots_C");
    void* settings  = SwitcherChild(switcher, L"ui_settings_C");
    void* fillDonor = DonorField(saveSlots, L"Image_0");
    void* barDonor  = DonorField(settings,  L"scrollboxRoot");
    // button_back joins the REQUIRED set rather than degrading to an unstyled button,
    // because the chrome it styles is the way OUT of this screen. An unstyled X that
    // still closes would be tolerable; a missing donor means we do not know what else
    // moved in this build, and the fail-closed rule exists for exactly that.
    void* backDonor = DonorField(saveSlots, L"button_back");
    if (!fillDonor || !barDonor || !backDonor) {
        if (++g_buildAttempts >= 15 && !g_toldTheUser) {
            g_toldTheUser = true;
            UE_LOGE("server_browser_native: donors still absent after %d attempts "
                    "(ui_saveSlots_C.Image_0=%p ui_settings_C.scrollboxRoot=%p "
                    "ui_saveSlots_C.button_back=%p) -- NOT building",
                    g_buildAttempts, fillDonor, barDonor, backDonor);
            ui::boot_warning_dialog::Arm(
                "Multivoid: the multiplayer screen could not be built.\n"
                "A required menu element was not found in this game build "
                "(ui_saveSlots.Image_0 / ui_settings.scrollboxRoot / "
                "ui_saveSlots.button_back).\n"
                "This usually means the game updated and the mod needs a new release.");
        }
        return false;
    }

    void* root = Spawn(P::name::UserWidgetClass, switcher);
    void* tree = root ? Spawn(P::name::WidgetTreeClass, root) : nullptr;
    void* ovl  = tree ? Spawn(L"Overlay", tree) : nullptr;
    if (!root || !tree || !ovl) return false;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(root) + P::off::UUserWidget_WidgetTree) = tree;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(tree) + P::off::UWidgetTree_RootWidget) = ovl;

    // (1) The SCRIM. Copied from the game: ui_saveSlots_C's own first child Image_302 is a
    // full-screen UImage with TintColor (0,0,0,0.5) and NO ResourceObject, and that is what
    // dims the menu behind every native sub-screen. It needs no donor, and being Visible is
    // what makes it absorb a click that misses the window.
    void* scrim = Spawn(L"Image", ovl);
    if (!scrim) return false;
    g_scrimW = scrim;
    U::SetImageTintRaw(scrim, FLinearColor{0.f, 0.f, 0.f, 0.5f});
    E::SetWidgetVisibility(scrim, 0);
    if (void* s = U::AddChild(ovl, scrim))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);

    // (2) The WINDOW: a centred SizeBox holding a fill image and the content column.
    void* winBox = Spawn(L"SizeBox", ovl);
    // THE WINDOW IS A FRAMED BOX, not a cloned 9-slice. Until 2026-08-26 this cloned
    // ui_saveSlots.Image_0's brush, which gave a soft borderless panel -- and every window
    // in VOTV's own menus is a 2 px #646464 frame around a #1A1A1A fill with sharp corners
    // (docs/VOTV_UI_STYLE.md section 3). The clone was the closest thing available before
    // anyone had measured the real treatment; now it is measured, so we author it.
    // `fillDonor` stays REQUIRED -- see the donor guard above; it is the canary for "did
    // this menu's class layout move", and dropping the check would trade a loud failure
    // for a silent one.
    void* winOvl = winBox ? AddFramedBox(winBox, kPanel, kBorderPx) : nullptr;
    void* col    = winOvl ? Spawn(L"VerticalBox", winOvl) : nullptr;
    if (!winBox || !winOvl || !col) return false;
    U::SetSizeBoxWidth(winBox, kWindowW);
    U::SetSizeBoxHeight(winBox, kWindowH);
    if (void* s = U::AddChild(winOvl, col)) {
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
        auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                             P::off::UOverlaySlot_Padding);
        pad[0] = pad[1] = pad[2] = pad[3] = kPadPx;
    }
    U::SetContent(winBox, winOvl);
    if (void* s = U::AddChild(ovl, winBox))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                        kCenter, kCenter);

    // (3) The content column: title, column headers, the list, the status line.
    // The title row is a BOX, not a lone text block, so the X has somewhere to sit. Until
    // 2026-08-26 this screen had no chrome at all and ESC was the only way out -- the
    // user's words were "I don't even see the X to close server browser window".
    // THE TITLE STRIP. Native windows put a centred white title on its own bordered strip
    // (style doc section 3), so the title gets a frame of its own rather than floating.
    // The X rides in the same strip at the right.
    if (void* titleBox = AddFramedBox(col, kPanel, kBorderPx)) {
        if (void* titleRow = Spawn(L"HorizontalBox", titleBox)) {
            // USER 2026-08-26: "The windows title should say something like Multivoid -
            // Server Browser and be in the style of votv, not the current colors." That
            // settles style doc section 6's open product call in the direction of the
            // game: native titles are WHITE, centred and larger, and the coop cyan appears
            // in no VOTV menu. The build identity the title used to carry is simply GONE
            // from this screen: the main menu shows "Multivoid <game> b<build>" in its top
            // left at all times, so a second copy inside the browser was redundant -- the
            // user said so after seeing it parked in the footer, which is where this first
            // moved it. The per-row Version column still carries what a player actually
            // needs here, which is each SERVER's pair, not ours.
            AddText(titleRow, L"Multivoid  -  Server Browser", 24, kText,
                              kJustCenter, 1.f);
            g_closeBtn = BuildButton(titleRow, backDonor, L"X", 20);
            if (void* s = U::AddChild(titleBox, titleRow))
                U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                                kFill, kCenter);
        }
        if (void* s = U::AddChild(col, titleBox)) {
            auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                                 P::off::UVerticalBoxSlot_Padding);
            pad[0] = pad[1] = pad[2] = 0.f; pad[3] = kPadPx;
        }
    }
    // The column header is the LIST's, not the window's: it must agree with the row fill
    // weights or the columns do not line up, so one module owns both.
    rows::BuildHeader(col);
    void* listBox = Spawn(L"SizeBox", col);
    void* list    = listBox ? Spawn(L"ScrollBox", listBox) : nullptr;
    if (!listBox || !list) return false;
    U::SetSizeBoxHeight(listBox, kListH);
    // The settings list's scrollbar treatment (section 7b): a server list is the long-list
    // case, and ui_saveSlots' own ScrollBox sets no bar style at all. NINE brushes.
    U::CloneStyle(list, P::off::UScrollBox_WidgetBarStyle, barDonor,
                  P::off::UScrollBox_WidgetBarStyle, P::off::FScrollBarStyle_Size,
                  P::off::FScrollBarStyleBrushes, 9);
    U::SetContent(listBox, list);
    if (void* s = U::AddChild(col, listBox))
        U::SetSlotAlign(s, P::off::UVerticalBoxSlot_HAlign, P::off::UVerticalBoxSlot_VAlign,
                        kFill, kFill);
    // The footer mirrors the title row: the status line takes the slack, BACK sits at the
    // right. Two exits, because the game's own sub-screens carry a button_back and a
    // player who does not think to press ESC should not be stranded.
    // THE FOOTER STRIP, and BACK sits at its LEFT.
    //
    // That placement is not a preference. Every native window that has both puts Back
    // bottom-LEFT and its actions bottom-RIGHT -- Settings is `Back | Reset all Apply`,
    // Keybinds is `Back | Hard reset`, the save browser is `Back` alone at the left. Our
    // first version put BACK bottom-right, which is the CONFIRM position, so a cancel
    // control sat where the game trains players to expect a commit (style doc section 5,
    // gap S7). Connect/Join will land at the right when T7 adds it, and then the row reads
    // the way every other VOTV window does.
    if (void* footBox = AddFramedBox(col, kPanel, kBorderPx)) {
        if (void* footRow = Spawn(L"HorizontalBox", footBox)) {
            g_backBtn = BuildButton(footRow, backDonor, L"BACK", 18);
            g_status  = AddText(footRow, L"", 16, kText, kJustLeft, 1.f);
            // The status has fill weight 1, so it takes all the slack and pushes these to
            // the right edge -- Back at the left, actions at the right, which is what every
            // native VOTV window does (style doc section 5, gap S7).
            if (!ui::server_browser_actions::Build(footRow, backDonor)) return false;
            if (void* s = U::AddChild(footBox, footRow))
                U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                                kFill, kCenter);
        }
        if (void* s = U::AddChild(col, footBox)) {
            auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                                 P::off::UVerticalBoxSlot_Padding);
            pad[0] = pad[1] = pad[3] = 0.f; pad[2] = kPadPx;
        }
    }

    g_root  = root;
    // Hand the list to its owner, which also drops every row identity from the menu
    // instance that just died -- those two effects are always wanted together.
    rows::Attach(list);
    UE_LOGI("server_browser_native: screen built (root=%p list=%p) after %d attempt(s)",
            root, list, g_buildAttempts + 1);
    return true;
}

void Show() {
    if (!g_switcher || !g_root || g_shown) return;
    g_priorIndex = U::SwitcherIndex(g_switcher);
    if (g_ourIndex < 0) {
        U::AddChild(g_switcher, g_root);
        g_ourIndex = U::ChildCount(g_switcher) - 1;
    }
    // The screen stays ATTACHED for the menu's life -- a switcher renders only its active
    // child, so an inactive 12th child costs nothing, and rebuilding N rows on every open
    // would churn GC for no reason. Only the index moves.
    U::SwitcherSetIndex(g_switcher, g_ourIndex);
    g_shown = true;
    g_escPrimed = false;   // re-prime: an ESC held while the screen opens must not close it
    g_lmbPrimed = false;   // ...and the same for the release that OPENED us
    rows::OnShown();       // ...and the hover, for the same reason: nothing else re-asks
    SyncRows();
    UE_LOGI("server_browser_native: shown (index %d -> %d)", g_priorIndex, g_ourIndex);
}

void Hide(const char* why) {
    if (!g_shown) return;
    g_shown = false;
    // Restore ONLY if the index is still ours: the game's own sibling screens write this
    // field to navigate, and stomping a navigation the player just made would be worse than
    // leaving it.
    const int32_t now = U::SwitcherIndex(g_switcher);
    if (now == g_ourIndex && g_priorIndex >= 0) U::SwitcherSetIndex(g_switcher, g_priorIndex);
    UE_LOGI("server_browser_native: hidden (%s; index was %d, ours %d)", why, now, g_ourIndex);
}

bool Armed() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::browser_native);
    return s;
}
bool AutoOpenArmed() {
    static const bool s =
        coop::config::ResolveFlag(::coop::config_registry::rows::browser_autoopen);
    return s && Armed();
}

}  // namespace

void Open() {
    g_wantOpenMs.store(::GetTickCount64(), std::memory_order_relaxed);
}

void Close() {
    g_wantOpenMs.store(0, std::memory_order_relaxed);
    // The hide touches the engine, so it must run on the GAME THREAD; this only records
    // the request and OnMenuTick performs it. Close() is called from the ImGui picker and
    // from the harness, neither of which is the game thread by construction.
    g_wantClose.store(true, std::memory_order_relaxed);
}

void CloseNow() {
    // ENFORCED, not merely documented. It sits one declaration below `Close()`, whose header
    // says "safe from any thread", and it reaches ProcessEvent through SwitcherSetIndex --
    // so the difference between them is exactly the kind a caller reads past. Off-thread it
    // degrades to the deferred close rather than touching the engine.
    if (!ue_wrap::game_thread::IsGameThread()) {
        UE_LOGW("server_browser_native: CloseNow off the game thread -- deferring instead "
                "(it drives the switcher through ProcessEvent)");
        Close();
        return;
    }
    g_wantOpenMs.store(0, std::memory_order_relaxed);
    g_wantClose.store(false, std::memory_order_relaxed);   // performed here, not deferred
    Hide("replaced by a sibling screen");
}

bool IsOpen() { return g_shown; }

// The three list questions are the LIST's to answer; this screen only forwards them, so
// that a caller who already holds `server_browser_native.h` need not learn a second header
// to ask what is selected.
int HoveredRow() { return rows::HoveredRow(); }
const char* SelectedRowId() { return rows::SelectedId(); }
bool SelectedRow(coop::net::lobby::LobbyRow& out) { return rows::Selected(out); }

void SetNotice(const char* text) {
    if (!text) return;
    g_noticeUntilMs = ::GetTickCount64() + kNoticeMs;
    if (!g_status) return;
    const std::string s(text);
    const std::wstring w = coop::text::FromUtf8Lossy(s.data(), s.size());
    E::SetWidgetText(g_status, w.c_str());
}

void LogRowHitDiagnostics(int32_t i) { rows::LogRowHitDiagnostics(i); }


void OnMenuTick(void* menu, void* switcher) {
    if (!Armed() || !menu || !switcher) return;
    g_switcher = switcher;

    // Rebuild on a new menu instance (the old widgets died with it).
    if (menu != g_menu) {
        g_menu = menu;
        g_root = nullptr; g_status = nullptr;
        g_closeBtn = nullptr; g_backBtn = nullptr; g_scrimW = nullptr;
        ui::server_browser_actions::Forget();
        g_ourIndex = -1; g_shown = false; g_buildAttempts = 0; g_toldTheUser = false;
        rows::Attach(nullptr);   // the panel died with the menu; drop it and the row ids
    }
    if (!g_root) {
        if (!BuildScreen(switcher)) return;
        if (AutoOpenArmed()) {
            UE_LOGW("server_browser_native: [dev] browser_autoopen=1 -- showing without a click");
            Open();
            selftest::Arm();
        }
    }
    g_menu = menu;

    // Consume the deferred intent. This IS the world gate: we are inside a MAIN-menu tick,
    // which is a first-hand positive observation that the menu is up and ticking -- strictly
    // stronger than any memoised world reading, and it cannot fire over gameplay.
    if (g_wantClose.exchange(false, std::memory_order_relaxed)) Hide("requested");
    const uint64_t want = g_wantOpenMs.load(std::memory_order_relaxed);
    if (want) {
        const uint64_t age = ::GetTickCount64() - want;
        if (age > kIntentTtlMs) {
            g_wantOpenMs.store(0, std::memory_order_relaxed);
            UE_LOGW("server_browser_native: open intent EXPIRED after %llu ms without a main "
                    "menu -- dropped rather than left armed", static_cast<unsigned long long>(age));
        } else {
            g_wantOpenMs.store(0, std::memory_order_relaxed);
            Show();
        }
    }

    // THE SELF-CHECK TICKS WHETHER OR NOT THE SCREEN IS SHOWN, and that is deliberate:
    // its last phases RE-OPEN the screen after ESC has closed it, so they can then drive
    // the X. Below the `!g_shown` return it would stop ticking the moment its own ESC
    // phase succeeded, and the chrome would stay untested forever. Every phase that needs
    // a visible screen runs before that point, in order.
    selftest::Tick(g_scrimW, rows::Panel(), g_closeBtn);

    if (!g_shown) return;

    // RECONCILE, do not assert. A sibling screen (or ESC reaching a stale `widgetEnter`,
    // which the game clears only on its own ESC path) can write ActiveWidgetIndex away from
    // ours; if that happened we were closed, whoever did it.
    if (U::SwitcherIndex(g_switcher) != g_ourIndex) {
        g_shown = false;
        UE_LOGI("server_browser_native: the switcher moved off our index -- treating as closed");
        return;
    }

    // ESC CLOSES THE SCREEN, and until the chrome exists this is the ONLY way out.
    //
    // The game's own ESC cannot help us: `ui_menu_C::OnKeyDown` casts `widgetEnter` to
    // int_widgets and then tests `ActiveWidgetIndex == 0`, and at our index BOTH fail, so
    // it is a measured no-op (section 8). That is fine for ui_saveSlots, which has a
    // button_back -- it was NOT fine here, where Close() had no callers at all and the
    // screen stranded the player at the menu with nothing to press. Exactly the hazard
    // section 8 wrote down for the RUNG 1 probe, which got a deadline and an auto-restore;
    // this got neither until 2026-08-26.
    //
    // Polled here rather than in the WndProc detour: this observer already runs every menu
    // tick, the poll costs one GetAsyncKeyState, and it keeps the change inside this TU --
    // no edit to the overlay's input path or to the one hands-on-verified inject. We do
    // not swallow the key; the game's handler runs too and is a no-op at our index (or
    // navigates away on a stale widgetEnter, which the reconcile below then observes).
    {
        const bool esc = (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if (!g_escPrimed) { g_escPrimed = true; g_prevEsc = esc; }
        const bool pressEdge = esc && !g_prevEsc;
        // (A one-shot 'ESC produced no edge' diagnostic stood here until 2026-08-29. It
        // was written to find why ESC did not close the screen; the cause turned out to
        // be that no input reached the game at all while an ImGui surface held capture,
        // which this could never have shown. RULE 2: it goes with its question.)
        g_prevEsc = esc;
        if (pressEdge) {
            Hide("ESC");
            return;
        }
    }

    // THE CHROME CLICK, on the LBUTTON RELEASE edge -- the same shape as the shipped,
    // hands-on-verified MULTIPLAYER button (multiplayer_menu.cpp:253-271) and for the same
    // reason: our buttons are real UButtons, so the mouse-DOWN drives Slate's Pressed
    // visual and acting on the down edge would close the screen out from under a button
    // that never saw its own release. Releasing lets it complete press->spring-back first.
    //
    // IsHovered() is a UFunction and is called ONLY on the release edge, never per frame.
    {
        const bool down = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (!g_lmbPrimed) { g_lmbPrimed = true; g_prevLmb = down; }
        const bool releaseEdge = !down && g_prevLmb;
        g_prevLmb = down;
        if (releaseEdge && ui::input_focus::IsOurWindowForeground()) {
            if (g_closeBtn && E::WidgetIsHovered(g_closeBtn)) { Hide("X"); return; }
            if (g_backBtn  && E::WidgetIsHovered(g_backBtn))  { Hide("BACK"); return; }
            // The action bar BEFORE the rows: its buttons sit in the footer, outside the
            // list, so they cannot both answer -- but returning here is what keeps a click
            // on CONNECT from also being read as a click on whatever is behind it.
            if (ui::server_browser_actions::OnReleaseEdge()) return;
            // A click on a hovered row SELECTS it. The row under the cursor is already
            // known from the hover pass, so this costs no extra dispatch. Returning on a
            // handled click matches the two lines above it -- the chrome and the action bar
            // both stop here -- so nothing below can read the same release a second time.
            if (rows::ClickSelect()) return;
        }
    }

    rows::UpdateHover();

    // FETCH ON A TIMER, PAINT ON AN ARRIVAL -- two questions, and they used to share one
    // gate. With paint coupled to the fetch tick, a lobby that arrived at t=0.3 s was not
    // drawn until t=5 s, and REFRESH called `sm::Refresh()` with no repaint at all, so the
    // button showed "Refreshing..." over an unchanged list and read as dead. `CopyRows`
    // already returns a generation that increments per completed fetch and `SyncRows` was
    // throwing it away; the sibling window has used exactly this shape (`g_savesRev`) since
    // it was written.
    const uint64_t now = ::GetTickCount64();
    if (now - g_lastRefreshMs >= kRefreshMs) {
        g_lastRefreshMs = now;
        sm::Refresh();
    }
    if (sm::RowsGeneration() != rows::PaintedGeneration()) SyncRows();
}

}  // namespace ui::server_browser_native
