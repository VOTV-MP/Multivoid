// ui/server_browser_native.cpp -- see ui/server_browser_native.h.

#include "ui/server_browser_native.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/session/session_manager.h"
#include "ui/boot_warning_dialog.h"     // the loud failure surface for a donor that never appears
#include "ui/input_focus.h"            // a click only counts while OUR window is foreground
#include "ui/native_screen.h"          // palette + widget primitives, shared with the host window
#include "ui/server_browser_actions.h"   // CONNECT / HOST / REFRESH, its own TU
#include "ui/server_browser_panels.h"    // the details panel + the black status pane
#include "ui/server_browser_rows.h"      // the LIST -- rows, identity, hover, selection
#include "ui/native_text_field.h"        // AnyFocused() -- a focused field owns Escape
#include "coop/dev/native_text_probe.h"   // the HALT rung: can a native field take text?
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
namespace panels = ui::server_browser_panels;

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
//
// EVERYTHING IN THE LEFT COLUMN COMES OUT OF THIS NUMBER, and there are three of them now:
// 470 originally, 440 when the action grid moved under the list, 396 when BACK joined them
// there (which is what let the right column's status pane run to the window's bottom edge
// instead of stopping above a full-width footer row). Two grid rows at 46 plus their gaps
// plus Back at 48 plus two 6 px separations is ~160; the body is ~564.
constexpr float kListH    = 396.f;
// The two columns of the body. The list is the subject and takes most of the width; the
// panels beside it hold prose, not a table, so they need enough to spell a sentence and no
// more. The save browser this mirrors splits about the same way.
constexpr float kListWeight   = 0.63f;
constexpr float kPanelsWeight = 0.37f;
// ---- the construction kit ----------------------------------------------------------
// Alignment enums, the palette, and the widget primitives moved to ui/native_screen
// (2026-08-29) when the hosting window became the second native screen. The names are
// re-bound here so every call site below reads exactly as it did -- the extraction is a
// MOVE, and a move that rewrites its call sites cannot be diffed against the original.
namespace NS = ui::native_screen;
using NS::kFill;
using NS::kCenter;
using NS::kBottom;
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
const FLinearColor kText   = NS::Text();    // the default: most text is white

// ---- state (GAME THREAD ONLY unless marked) --------------------------------------
void* g_menu     = nullptr;   // the ui_menu_C we built into (compared, never dereferenced)
void* g_switcher = nullptr;
void* g_root     = nullptr;   // our UUserWidget
void* g_scrimW   = nullptr;   // the full-screen scrim -- the thing that absorbs a stray click
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
// How long the DEV autoopen waits after building before it shows the screen. It exists to
// make the lab reproduce a player's timing -- long enough for the forced collection to run
// and for the unreferenced-tree bug to bite if it is ever reintroduced. ~60 menu ticks is
// half a second at this menu's measured frame rate; the whole browser scenario budgets 140.
constexpr int kAutoOpenDelayTicks = 60;
int g_autoOpenIn = 0;
int32_t g_priorIndex = -1;
bool    g_shown      = false;

uint64_t g_lastRefreshMs = 0;
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

// Pull the list, then repaint the panes beside it -- in that order, because the details
// panel resolves its subject against the rows this pass just wrote.
//
// THE ONE-LINE FOOTER IS GONE (RULE 2, 2026-08-31). It carried `sm::Status()` and, for six
// seconds at a time, a click's answer written over it -- one line for two facts, so the
// answer erased the server count and the count erased the answer. The status PANE has a
// line for each (ui/server_browser_panels), so `RefreshStatusLine` and the notice deadline
// it fought with went with the footer rather than being left driving a widget nobody builds.
void SyncRows() {
    rows::Sync();
    panels::Sync(true);
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
    // The FRAME donor. NOT in the required set: a missing bevel is cosmetic, while every
    // donor above decides whether the screen works at all. `[V]` ui_settings.image_border
    // carries the material `inst_uiBorder` as a 9-slice box with Margin 0.5 -- the thing our
    // flat rectangle was imitating and could not match, because each native edge has its own
    // pair of greys. Set before ANY AddFramedBox call below.
    // ONCE PER MENU INSTANCE, not once per retry tick. DonorChild costs a full GUObjectArray
    // walk, and BuildScreen re-runs on EVERY menu tick until the screen builds -- so an
    // unlatched resolve here is a ~117 Hz array walk for as long as anything else is failing.
    if (!NS::BorderDonorResolved()) {
        if (void* borderDonor = NS::DonorChild(settings, L"image_border")) {
            // Only a NON-NULL donor is published. Setting it unconditionally meant one tick
            // where `settings` was momentarily unresolved overwrote a good donor with null for
            // every sibling screen, permanently for that menu instance.
            NS::SetBorderDonor(borderDonor);
        } else if (!g_toldTheUser) {
            // WARN once, not per tick: log.cpp fflushes every non-INFO line synchronously, and
            // this sits on a path re-entered until the screen builds.
            UE_LOGW("server_browser_native: frame donor ui_settings.image_border NOT found -- "
                    "windows fall back to the flat border (cosmetic, not fatal)");
        }
    }
    if (!fillDonor || !barDonor || !backDonor) {
        // The caller counts the attempt now (every failure path, not just this one), so this
        // only READS the counter. Incrementing here too would double-count this path and arm
        // the dialog at 8 attempts instead of 15.
        if (g_buildAttempts >= 15 && !g_toldTheUser) {
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

    // (1..3) THE SHELL -- the switcher child, its widget tree, the scrim, the centred
    // framed window and the title strip -- all from the shared kit since 2026-08-31.
    // Three screens carried byte-identical copies of it; see `native_screen.h`.
    //
    // NO X, ON EITHER WINDOW (USER 2026-08-30: "не надо крестиков значит. Пусть окна
    // закрывает юзер также как и нативные менюшки votv"). No native VOTV window has one,
    // and MTA's own frame X is enabled with NO handler behind it
    // (CServerBrowser.cpp -- SetCloseClickHandler is called nowhere in their core), so
    // both precedents point the same way. It went for FIDELITY, not because it failed:
    // `CLOSE BUTTON PASS` and the sibling's `HOST X PASS` were both measured on
    // 2026-08-30 at 23:43, hours before it was removed. What replaces it is Back and ESC,
    // and those are what the self-check now drives.
    //
    // The title says what the user asked it to say on 2026-08-26 ("Multivoid - Server
    // Browser ... in the style of votv, not the current colors"). It carries no build
    // identity: the main menu shows "Multivoid <game> b<build>" in its top left at all
    // times, and each SERVER's pair is what a player needs here, which the details panel
    // spells out.
    //
    // `fillDonor` stays REQUIRED in the guard above even though the window authors its own
    // frame now: it is the canary for "did this menu's class layout move", and dropping
    // the check would trade a loud failure for a silent one.
    NS::WindowShell shell;
    if (!NS::BuildWindowShell(switcher, kWindowW, kWindowH,
                              L"Multivoid  -  Server Browser", shell))
        return false;
    void* root = shell.root;
    void* col  = shell.column;
    g_scrimW   = shell.scrim;
    // (4) THE BODY: the list on the LEFT, the two panes on the RIGHT.
    //
    // This is the redesign, and the shape is not ours -- it is VOTV's own save browser
    // (`docs/SERVER_BROWSER_ARC.md` section 7.1), the game's one screen for "browse things
    // and act on one". List left, what-you-picked top-right, a black status pane under it,
    // the actions in a block beneath the list, `Back` alone at the bottom left. What it
    // replaces is a five-column table with a one-line footer, which the user rejected whole
    // ("это дизайн говно у сервер браузера ... нужен дизайн сервер браузера как у людей").
    void* body = Spawn(L"HorizontalBox", col);
    if (!body) return false;
    NS::AddVFill(col, body, 1.f, kFill, kFill);

    void* leftCol = Spawn(L"VerticalBox", body);
    if (!leftCol) return false;
    if (void* s = NS::AddHFill(body, leftCol, kListWeight, kFill, kFill))
        NS::SetSlotPadding(s, P::off::UHorizontalBoxSlot_Padding, 0.f, 0.f, kPadPx, 0.f);

    void* listBox = Spawn(L"SizeBox", leftCol);
    void* list    = listBox ? Spawn(L"ScrollBox", listBox) : nullptr;
    if (!listBox || !list) return false;
    U::SetSizeBoxHeight(listBox, kListH);
    // The settings list's scrollbar treatment (section 7b): a server list is the long-list
    // case, and ui_saveSlots' own ScrollBox sets no bar style at all. NINE brushes.
    U::CloneStyle(list, P::off::UScrollBox_WidgetBarStyle, barDonor,
                  P::off::UScrollBox_WidgetBarStyle, P::off::FScrollBarStyle_Size,
                  P::off::FScrollBarStyleBrushes, 9);
    U::SetContent(listBox, list);
    NS::AddVFill(leftCol, listBox, 1.f, kFill, kFill);

    // THE ACTION GRID, directly under the list it acts on.
    if (void* gridWrap = Spawn(L"VerticalBox", leftCol)) {
        if (void* s = NS::AddVFill(leftCol, gridWrap, 0.f, kFill, kBottom))
            NS::SetSlotPadding(s, P::off::UVerticalBoxSlot_Padding, 0.f, kPadPx, 0.f, 0.f);
        if (!ui::server_browser_actions::Build(gridWrap, backDonor)) return false;
    } else {
        return false;
    }

    // BACK, ALONE AT THE BOTTOM LEFT -- INSIDE THE LEFT COLUMN, not in a row beneath both.
    //
    // The placement itself is not a preference: every native window that has both puts Back
    // bottom-LEFT and its actions bottom-RIGHT -- Settings is `Back | Reset all Apply`, the
    // save browser is `Back` ALONE at the left (style doc section 5, gap S7).
    //
    // WHICH CONTAINER it sits in is what changed, and the user found the reason by eye: a
    // full-width footer row under the body ended both columns above it, so the whole band
    // to the RIGHT of Back was empty -- "под правой панелью внизу неиспользуемое место
    // пустое" (2026-08-31, with the region circled). Putting Back in the left column makes
    // the body the only thing between the title and the window's bottom edge, so the status
    // pane's Fill slot runs all the way down and there is no dead band to leave.
    if (void* footRow = Spawn(L"HorizontalBox", leftCol)) {
        // Sentence case: VOTV uppercases no button label anywhere (measured across the
        // style corpus; user report 2026-08-30 "No caps at buttons ever").
        g_backBtn = BuildButton(footRow, backDonor, L"Back", ui::native_screen::kBtnFontPx);
        if (!g_backBtn) return false;
        NS::SetHSlot(NS::SlotOf(g_backBtn), 0.f, NS::kLeft, kCenter);
        if (void* s = NS::AddVFill(leftCol, footRow, 0.f, NS::kLeft, kBottom))
            NS::SetSlotPadding(s, P::off::UVerticalBoxSlot_Padding, 0.f, kPadPx, 0.f, 0.f);
    }

    void* rightCol = Spawn(L"VerticalBox", body);
    if (!rightCol) return false;
    NS::AddHFill(body, rightCol, kPanelsWeight, kFill, kFill);
    if (!panels::BuildDetails(rightCol)) return false;
    // CONNECT SITS DIRECTLY UNDER THE PANEL THAT DESCRIBES WHAT IT WILL JOIN (USER
    // 2026-08-31). The rest of the actions stay in the grid beneath the list, because they
    // are the ones that do not depend on which row is chosen.
    if (!ui::server_browser_actions::BuildConnect(rightCol, backDonor)) return false;
    if (!panels::BuildStatus(rightCol)) return false;

    g_root  = root;
    // Hand the list to its owner, which also drops every row identity from the menu
    // instance that just died -- those two effects are always wanted together.
    rows::Attach(list);

    // ATTACH NOW, NOT AT FIRST Show(). NOTHING ELSE REFERENCES THIS TREE.
    //
    // The attach used to live in `Show()`, so between building the screen and the player's
    // first click the whole subtree was an unreferenced UObject graph -- and UE's garbage
    // collector took it. `AddChild` then returned null on a dead object, and before the
    // index was proven that produced a switch to one of the GAME's own screens (the user
    // clicked MULTIPLAYER and got VOTV's Stats panel); after it was proven, the button went
    // dead instead. Same root, two faces.
    //
    // WHY NO LAB RUN EVER SAW IT: every automated scenario sets `browser_autoopen=1`, which
    // calls Show() on the SAME TICK as the build. The gap the bug lives in is exactly the
    // gap a human takes to move the mouse. (MEASURED 2026-08-30 -- `AddChild
    // slot=0000000000000000` in a hands-on log carrying 41 GC lines in the same window.)
    //
    // Attaching here is also what the code already claimed to do: Show()'s own comment says
    // "the screen stays ATTACHED for the menu's life". It just did not become true until the
    // first open. `AddToRoot` is the wrong tool -- a switcher child is reachable from the
    // menu, which is the reference we actually want (RUNG 2 measured that a hand-built
    // subtree survives a forced GC once it is IN the tree).
    {
        void* slot = U::AddChild(g_switcher, g_root);
        g_ourIndex = U::IndexOfChild(g_switcher, g_root);
        if (g_ourIndex < 0) {
            UE_LOGE("server_browser_native: built the server browser but could NOT place it in the menu switcher "
                    "(AddChild slot=%p, GetChildIndex=-1). The screen cannot be shown this "
                    "menu; it will be rebuilt on the next one.", slot);
            ue_wrap::log::Flush();
            g_root = nullptr;   // force a rebuild rather than keep an unreachable tree
            return false;
        }
    }
    UE_LOGI("server_browser_native: screen built (root=%p list=%p) after %d attempt(s)",
            root, list, g_buildAttempts + 1);
    return true;
}

void Show() {
    if (!g_switcher || !g_root || g_shown) return;
    // The index was proven when the screen was BUILT and attached; if that had failed,
    // `g_root` was cleared and we never get here.
    g_priorIndex = U::SwitcherIndex(g_switcher);
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

// The status PANE owns the notice line now; this stays as the entry point every action
// already calls, so the action TU keeps talking to the screen and not to its furniture.
void SetNotice(const char* text) { panels::SetNotice(text); }

void LogRowHitDiagnostics(int32_t i) { rows::LogRowHitDiagnostics(i); }


void OnMenuTick(void* menu, void* switcher) {
    if (!Armed() || !menu || !switcher) return;
    g_switcher = switcher;

    // Rebuild on a new menu instance (the old widgets died with it).
    if (menu != g_menu) {
        g_menu = menu;
        g_root = nullptr;
        g_backBtn = nullptr; g_scrimW = nullptr;
        ui::server_browser_actions::Forget();
        panels::Forget();
        g_ourIndex = -1; g_shown = false; g_buildAttempts = 0; g_toldTheUser = false;
        rows::Attach(nullptr);   // the panel died with the menu; drop it and the row ids
        // The frame donor is a UImage owned by the OLD menu's ui_settings. Kept across the
        // rebuild, CloneStyle would memcpy 0x88 bytes out of a destroyed widget -- and every
        // other per-instance pointer beside it was already being dropped here.
        NS::ForgetBorderDonor();
    }
    if (!g_root) {
        // COUNT THE ATTEMPT IN THE CALLER. BuildScreen increments only inside its
        // missing-donor guard, so its ~13 other `return false` paths -- including the AddChild
        // failure measured live on 2026-08-30, which deliberately clears g_root to force a
        // rebuild -- retried at menu-tick rate with no backoff and never armed the dialog.
        // host_window_native does this in its caller and its comment claims both siblings do;
        // that claim was false for this one.
        ++g_buildAttempts;
        if (!BuildScreen(switcher)) return;
        if (AutoOpenArmed()) {
            // THE AUTOOPEN DELIBERATELY DOES **NOT** OPEN ON THIS TICK, AND THAT IS THE
            // WHOLE POINT OF THE DELAY.
            //
            // It used to call Open() here, in the same tick that built the screen -- which
            // made every automated run take a path no player can take, and hid a real bug
            // for days: the tree was attached to the switcher lazily on first Show, so
            // between the build and a human's click it was an unreferenced UObject graph
            // that GC collected. Opening immediately left no gap for GC, so the lab was
            // green while the shipped button opened VOTV's Stats panel and then, once the
            // index was proven, did nothing at all.
            //
            // So the dev path now walks the same shape a person does: build, force a
            // collection, let ticks pass, THEN open. An instrument that only exercises the
            // privileged timing is an instrument blind to the phenomenon, and this project
            // has a lesson by that name.
            UE_LOGW("server_browser_native: [dev] browser_autoopen=1 -- forcing a GC and "
                    "opening in %d ticks, so the lab walks the same build-then-click gap a "
                    "player does", kAutoOpenDelayTicks);
            E::ForceGarbageCollection();
            g_autoOpenIn = kAutoOpenDelayTicks;
        }
    }
    if (g_autoOpenIn > 0 && --g_autoOpenIn == 0) {
        Open();
        selftest::Arm();
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
    selftest::Tick(g_scrimW, rows::Panel(), g_backBtn);

    // THE HALT RUNG (2026-08-30). Dev-gated and latched; does nothing for a player.
    // It rides this tick because it needs the browser's own panel, which is the tree
    // whose behaviour is in question -- a probe against a tree nobody ships would
    // answer about a different tree.
    coop::dev::native_text_probe::Tick(rows::Panel());



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
        // A FOCUSED TEXT FIELD OWNS ESCAPE, and this poll is why the field cannot claim it
        // by swallowing the message: GetAsyncKeyState reads the PHYSICAL key, so consuming
        // WM_KEYDOWN in the detour would still leave this edge firing -- one press would
        // blur the field AND close the screen. The field's own handler turns Escape into
        // "leave the field"; this defers to it for exactly that press.
        if (pressEdge && ui::native_text_field::AnyFocused()) return;
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
            // IsHovered, and it is RIGHT here: these are real UButtons and they answer.
            // Converting them to geometry on 2026-08-30 turned a passing X into
            // CLOSE BUTTON FAIL in one run -- the clearest possible evidence that the
            // two widget kinds need the two different questions.
            if (g_backBtn  && E::WidgetIsHovered(g_backBtn))  { Hide("BACK"); return; }
            // The action bar BEFORE the rows: its buttons sit in the footer, outside the
            // list, so they cannot both answer -- but returning here is what keeps a click
            // on CONNECT from also being read as a click on whatever is behind it.
            if (ui::server_browser_actions::OnReleaseEdge()) return;
            // A click on a hovered row SELECTS it. The row under the cursor is already
            // known from the hover pass, so this costs no extra dispatch. Returning on a
            // handled click matches the two lines above it -- the chrome and the action bar
            // both stop here -- so nothing below can read the same release a second time.
            //
            // The details panel is FORCED here rather than left to the 1 Hz repaint: the
            // player just chose a server and the panel is the answer to that click. A pane
            // that fills in up to a second later reads as a click that did not register.
            if (rows::ClickSelect()) { panels::Sync(true); return; }
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

    // The panes carry two SECONDS counters ("updated N s ago", "Last seen N s"), so they
    // repaint on their own 1 Hz cadence between fetches. Every line writes only when its
    // text changed, so a tick where nothing moved costs the comparisons and no dispatch.
    panels::Sync(false);
}

}  // namespace ui::server_browser_native
