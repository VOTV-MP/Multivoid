// ui/server_browser_native.cpp -- the native server browser screen: built once per menu
// instance from game donors, attached to the menu's switcher, driven from the menu tick (ESC,
// the release-edge click, the hover, the 5 s fetch). See ui/server_browser_native.h.

#include "ui/server_browser_native.h"

#include "l10n/l10n.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/session/session_manager.h"
#include "coop/text/utf8_codec.h"       // a looked-up (UTF-8) label, widened for the native widgets
#include "ui/boot_warning_dialog.h"    // the loud failure surface for a donor that never appears
#include "ui/input_focus.h"            // a click only counts while OUR window is foreground
#include "ui/native_screen.h"          // palette + widget primitives, shared with the host window
#include "ui/server_browser_actions.h"   // CONNECT / HOST / REFRESH, its own TU
#include "ui/server_browser_panels.h"    // the details panel + the black status pane
#include "ui/server_browser_rows.h"      // the LIST -- rows, identity, hover, selection
#include "ui/server_browser_tabs.h"      // the master tabs above it
#include "coop/net/master_slots.h"       // which master a switch landed on, for the notice
#include "ui/native_text_field.h"        // AnyFocused() -- a focused field owns Escape
#include "coop/dev/native_text_probe.h"   // the HALT rung: can a native field take text?
#include "ui/server_browser_selftest.h"  // the dev phase machine; ships dark
#include "ui/server_browser_surface.h"   // the dev autoopen opens the way the button does
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
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

// Slate units, the window's own; the row metrics live with the rows. This is the size the layout
// is measured at and the least the player can drag it to; the window grows from it.
constexpr float kWindowW  = 980.f;
constexpr float kWindowH  = 662.f;
// Frame and spacing, from the native windows.
constexpr float kBorderPx = 2.f;
constexpr float kPadPx    = 6.f;
// The list's height is explicit, not the VerticalBox's leftover slack: with a Fill slot the box
// allotted the ScrollBox more than the window had left, so the list overflowed upward and its
// first row drew clipped under the header. Everything in the left column comes out of this
// number: the master tabs at 38 plus the kit's 2 px list gap, two grid rows at 46 plus their
// gaps, Back at 48 and two 6 px separations is about 200, and the body is about 606; the fit
// probe reads what is left over.
constexpr float kListH    = 396.f;
// The two body columns: the list is the subject; the panes hold prose and need enough to spell a
// sentence. The save browser this mirrors splits about 63/37. The panes keep the width that split
// gives them at kWindowW; a wider window widens the list, whose server names are what grows.
constexpr float kPanelsW = 354.f;
// The resize grip, the window's bottom right corner: the area that takes the press, and its inset
// from the frame's outer edge.
constexpr float kGripPx    = 18.f;
constexpr float kGripInset = 2.f;
// The construction kit (alignment enums, palette, widget primitives) is ui/native_screen, shared
// with the hosting window; the names are re-bound so the call sites read unchanged.
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

// Only the window's colours; the row palette lives with the rows.
const FLinearColor kPanel  = NS::Panel();   // window fill
const FLinearColor kText   = NS::Text();    // the default: most text is white

// State, game thread only unless marked.
void* g_menu     = nullptr;   // the ui_menu_C we built into (compared, never dereferenced)
void* g_switcher = nullptr;
void* g_root     = nullptr;   // our UUserWidget
void* g_scrimW   = nullptr;   // the full-screen scrim -- the thing that absorbs a stray click
void* g_backBtn  = nullptr;   // BACK, bottom left, the lowest widget of the list's column
void* g_frame    = nullptr;   // the window's frame, what the fit probe measures against
void* g_listBox  = nullptr;   // the list's SizeBox: its height is the window's less the rest
void* g_grip     = nullptr;   // the resize grip's box, bottom right
// Whether the list's column (the master tabs, the list, the actions, Back) fits the window.
NS::FitProbe g_fit("server_browser_native", kWindowH);
// The window's size, in Slate units: read from the config once, then the player's drag. Kept
// across menu instances, so a rebuilt browser opens at the size it was left at.
float g_winW = 0.f, g_winH = 0.f;
// A drag of the grip: where it started (the cursor, in widget space; the window's size), and the
// widget space's pixels per Slate unit, read off the frame when it started.
bool  g_dragging = false;
long  g_dragX0 = 0, g_dragY0 = 0;
float g_dragW0 = 0.f, g_dragH0 = 0.f, g_dragScale = 1.f;
// LBUTTON edge state for the chrome poll, primed on Show so the release that opened the screen
// is not read as a click on whatever sits under the cursor.
bool  g_prevLmb   = false;
bool  g_lmbPrimed = false;

// ESC edge state, primed on the first shown tick so a key already held cannot synthesise a
// close.
bool  g_prevEsc   = false;
bool  g_escPrimed = false;
int32_t g_ourIndex   = -1;
// How long the dev autoopen waits after building before it shows the screen: long enough for
// the forced collection to run and for an unreferenced tree to be collected. About 60 menu
// ticks is half a second at this menu's frame rate; the browser scenario budgets 140.
constexpr int kAutoOpenDelayTicks = 60;
int g_autoOpenIn = 0;
int32_t g_priorIndex = -1;
bool    g_shown      = false;

uint64_t g_lastRefreshMs = 0;
int      g_buildAttempts = 0;
bool     g_toldTheUser   = false;

// Cross-thread: the deferred open and close intents.
std::atomic<uint64_t> g_wantOpenMs{0};   // 0 = no intent
std::atomic<bool>     g_wantClose{false};
constexpr uint64_t kIntentTtlMs  = 20000;  // a join that never returns to a menu must expire
// The re-fetch cadence. It gates sm::Refresh, which spawns a detached thread and a TLS
// handshake per fetch, for a list that changes on the scale of a person deciding to host.
constexpr uint64_t kRefreshMs    = 5000;

// The list first, then the panes: the details panel resolves its subject against the rows this
// pass wrote. The status pane has a line each for the server count and a click's answer, so
// neither erases the other.
void SyncRows() {
    rows::Sync();
    panels::Sync(true);
}

void LoadWindowSize() {
    if (g_winW > 0.f) return;
    const long w = coop::config::ResolveInt(::coop::config_registry::rows::browser_width);
    const long h = coop::config::ResolveInt(::coop::config_registry::rows::browser_height);
    g_winW = (std::max)(kWindowW, static_cast<float>(w));
    g_winH = (std::max)(kWindowH, static_cast<float>(h));
}

// The size in force: the frame, and the list's height, which takes every unit the window grew by.
void ApplyWindowSize() {
    if (g_frame) {
        U::SetSizeBoxWidth(g_frame, g_winW);
        U::SetSizeBoxHeight(g_frame, g_winH);
    }
    if (g_listBox) U::SetSizeBoxHeight(g_listBox, kListH + (g_winH - kWindowH));
    g_fit.SetWindowH(g_winH);
}

// The largest the window may be: the screen it sits on, in the same Slate units (the scrim fills
// the screen). Unmeasured, only the config row's own ceiling holds.
void MaxWindowSize(float& maxW, float& maxH) {
    maxW = 7680.f; maxH = 4320.f;
    ue_wrap::FVector2D sz{};
    if (g_scrimW && U::WidgetLocalSize(g_scrimW, sz) && sz.X > kWindowW && sz.Y > kWindowH) {
        maxW = sz.X; maxH = sz.Y;
    }
}

void SaveWindowSize() {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%ld", static_cast<long>(g_winW + 0.5f));
    const auto w = coop::config::SetValue(::coop::config_registry::rows::browser_width, buf);
    std::snprintf(buf, sizeof(buf), "%ld", static_cast<long>(g_winH + 0.5f));
    const auto h = coop::config::SetValue(::coop::config_registry::rows::browser_height, buf);
    // The cursor beside the frame's corner: a drag that tracks ends with the two together, to
    // within the grip's own offset from the corner (or apart where a limit stopped the window).
    long cx = 0, cy = 0;
    ue_wrap::FVector2D tl{}, sz{};
    NS::CursorInWidgetSpace(cx, cy);
    U::WidgetScreenRect(g_frame, tl, sz);
    UE_LOGI("server_browser_native: resized to %.0f x %.0f (%s; cursor %ld,%ld, frame corner "
            "%.0f,%.0f)", g_winW, g_winH,
            (w == coop::config::SetResult::Saved && h == coop::config::SetResult::Saved)
                ? "saved" : "NOT saved; this session only",
            cx, cy, tl.X + sz.X, tl.Y + sz.Y);
}

// The grip: small squares in three steps rising to the corner, the usual resize mark, drawn from
// tinted rects like the rows' padlock. The SizeBox is what a press is tested against.
void* BuildGrip(void* frame) {
    void* box = frame ? Spawn(L"SizeBox", frame) : nullptr;
    void* ovl = box ? Spawn(L"Overlay", box) : nullptr;
    if (!box || !ovl) return nullptr;
    U::SetSizeBoxWidth(box, kGripPx);
    U::SetSizeBoxHeight(box, kGripPx);
    // {left, top} of each 3x3 square in the 18x18 box, the last row and column 1 unit in from its
    // right and bottom edges.
    const float kDots[6][2] = {{14.f, 6.f}, {10.f, 10.f}, {14.f, 10.f},
                               {6.f, 14.f}, {10.f, 14.f}, {14.f, 14.f}};
    for (const auto& d : kDots) {
        void* img = Spawn(L"Image", ovl);
        if (!img) return nullptr;
        U::SetImageTintRaw(img, NS::Dim());
        E::SetWidgetVisibility(img, 3);   // HitTestInvisible
        if (void* s = U::AddChild(ovl, img)) {
            U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
            NS::SetSlotPadding(s, P::off::UOverlaySlot_Padding, d[0], d[1],
                               kGripPx - d[0] - 3.f, kGripPx - d[1] - 3.f);
        }
    }
    U::SetContent(box, ovl);
    // Added after the column, so it draws over the status pane's corner.
    if (void* s = U::AddChild(frame, box)) {
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                        NS::kRight, kBottom);
        NS::SetSlotPadding(s, P::off::UOverlaySlot_Padding, 0.f, 0.f, kGripInset, kGripInset);
    }
    return box;
}

// A press on the grip starts a drag; while the button is held the window follows the cursor; the
// release ends it and saves the size. The window stays centred, so its corner moves half as far as
// its size changes: the size changes by twice the cursor's travel, which keeps the corner under it.
void StartDrag(long cx, long cy) {
    ue_wrap::FVector2D tl{}, sz{};
    if (!U::WidgetScreenRect(g_frame, tl, sz) || sz.X < 1.f || g_winW < 1.f) return;
    g_dragScale = sz.X / g_winW;
    g_dragX0 = cx; g_dragY0 = cy;
    g_dragW0 = g_winW; g_dragH0 = g_winH;
    g_dragging = true;
    UE_LOGI("server_browser_native: resize started at %.0f x %.0f (cursor %ld,%ld; frame %.0f,%.0f "
            "+%.0fx%.0f px, %.3f px a unit)", g_winW, g_winH, cx, cy, tl.X, tl.Y, sz.X, sz.Y,
            g_dragScale);
}

void UpdateDrag() {
    long cx = 0, cy = 0;
    if (!NS::CursorInWidgetSpace(cx, cy) || g_dragScale <= 0.f) return;
    float maxW = 0.f, maxH = 0.f;
    MaxWindowSize(maxW, maxH);
    const float w = std::clamp(g_dragW0 + 2.f * static_cast<float>(cx - g_dragX0) / g_dragScale,
                               kWindowW, maxW);
    const float h = std::clamp(g_dragH0 + 2.f * static_cast<float>(cy - g_dragY0) / g_dragScale,
                               kWindowH, maxH);
    // Whole units, and only on a change: each write is a reflected call and a layout pass.
    const float rw = std::floor(w), rh = std::floor(h);
    if (rw == g_winW && rh == g_winH) return;
    g_winW = rw; g_winH = rh;
    ApplyWindowSize();
}

// Builds the screen once per menu instance. Fail closed: a null donor means no build and a
// retry, never a default style (that fallback is the Roboto, centred, white bug); after enough
// attempts the player is told, since a silent forever-retry is the same defect one level
// quieter.
bool BuildScreen(void* switcher) {
    // Backed off once the player has been told: each attempt costs two SwitcherChild walks (an
    // engine alloc and a wstring per child) and three DonorField lookups that render a name per
    // property while climbing SuperStruct, at ~117 menu ticks a second, on the path a version
    // migration lands on. Once a second is plenty.
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
    // button_back is required rather than degrading to an unstyled button: its chrome is the way
    // out of the screen, and a missing donor means the build's layout moved.
    void* backDonor = DonorField(saveSlots, L"button_back");
    // The frame donor, not required: a missing bevel is cosmetic, while every donor above decides
    // whether the screen works. Set before any AddFramedBox call, and resolved once per menu
    // instance: DonorChild walks the whole GUObjectArray, and BuildScreen re-runs every menu tick
    // until the screen builds.
    if (!NS::BorderDonorResolved()) {
        if (void* borderDonor = NS::DonorChild(settings, L"image_border")) {
            // Only a non-null donor is published: one tick with `settings` momentarily unresolved
            // once overwrote a good donor with null for every sibling screen, for the whole menu
            // instance.
            NS::SetBorderDonor(borderDonor);
            // Logged once per menu instance (BorderDonorResolved latches it): with only the failure
            // logged, a run that never opened the browser looked like one that framed everything.
            UE_LOGI("server_browser_native: frame donor ui_settings.image_border resolved "
                    "(%p) -- windows get the game's own 9-slice bevel", borderDonor);
        } else if (!g_toldTheUser) {
            // WARN once, not per tick: every non-INFO line flushes synchronously, and this path
            // re-enters until the screen builds.
            UE_LOGW("server_browser_native: frame donor ui_settings.image_border NOT found -- "
                    "windows fall back to the flat border (cosmetic, not fatal)");
        }
    }
    if (!fillDonor || !barDonor || !backDonor) {
        // The caller counts the attempt (every failure path, not only this one); counting here too
        // would arm the dialog at 8 attempts instead of 15.
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

    // The shell (the switcher child, its widget tree, the scrim, the centred framed window, the
    // title strip) comes from the shared kit. No X on either window: no native VOTV window has one,
    // and MTA's frame X has no handler behind it (CServerBrowser.cpp); Back and ESC close the
    // screen. The title carries no build identity: the main menu shows the mod's pair top left, and
    // each server's pair is in the details panel. fillDonor stays required although the window
    // authors its own frame: it is the canary for a moved menu layout.
    LoadWindowSize();
    NS::WindowShell shell;
    if (!NS::BuildWindowShell(switcher, g_winW, g_winH,
                              coop::text::FromUtf8Lossy(l10n::T("Multivoid  -  Server Browser")).c_str(), shell))
        return false;
    void* root = shell.root;
    void* col  = shell.column;
    g_scrimW   = shell.scrim;
    g_frame    = shell.box;
    // The body, VOTV's own save-browser shape: the list on the left, what you picked top right, a
    // black status pane under it, the actions beneath the list, Back alone at the bottom left.
    void* body = Spawn(L"HorizontalBox", col);
    if (!body) return false;
    NS::AddVFill(col, body, 1.f, kFill, kFill);

    void* leftCol = Spawn(L"VerticalBox", body);
    if (!leftCol) return false;
    if (void* s = NS::AddHFill(body, leftCol, 1.f, kFill, kFill))
        NS::SetSlotPadding(s, P::off::UHorizontalBoxSlot_Padding, 0.f, 0.f, kPadPx, 0.f);

    // The master tabs sit on the list they choose, the way MTA's source tabs sit on theirs.
    if (!ui::server_browser_tabs::Build(leftCol)) return false;

    // The list wears its own frame, as in the game: on the native Keybinds window the list panel's
    // ring sits flush against the window's. The ScrollBox goes inside the framed overlay; the
    // SizeBox still bounds the height, so the row layout's allotment is unchanged.
    void* listBox = Spawn(L"SizeBox", leftCol);
    void* listOvl = listBox ? AddFramedBox(listBox, NS::Panel(), kBorderPx) : nullptr;
    void* list    = listOvl ? Spawn(L"ScrollBox", listOvl) : nullptr;
    if (!listBox || !listOvl || !list) return false;
    g_listBox = listBox;   // its height is set with the window's, below
    // The settings list's scrollbar treatment: a server list is the long-list case, and
    // ui_saveSlots' own ScrollBox sets no bar style. Nine brushes.
    U::CloneStyle(list, P::off::UScrollBox_WidgetBarStyle, barDonor,
                  P::off::UScrollBox_WidgetBarStyle, P::off::FScrollBarStyle_Size,
                  P::off::FScrollBarStyleBrushes, 9);
    if (void* s = U::AddChild(listOvl, list)) {
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
        // Inset by the ring's rendered width (4 px), not by kBorderPx (the 2 px the flat fallback
        // insets its fill by): at 2 the rows' own outer band lands on the list ring's inner band
        // and doubles the light run at the list's edges.
        NS::SetSlotPadding(s, P::off::UOverlaySlot_Padding,
                           NS::kNativeRingPx, NS::kNativeRingPx,
                           NS::kNativeRingPx, NS::kNativeRingPx);
    }
    U::SetContent(listBox, listOvl);
    NS::AddVFill(leftCol, listBox, 1.f, kFill, kFill);

    // The action grid, directly under the list it acts on.
    if (void* gridWrap = Spawn(L"VerticalBox", leftCol)) {
        if (void* s = NS::AddVFill(leftCol, gridWrap, 0.f, kFill, kBottom))
            NS::SetSlotPadding(s, P::off::UVerticalBoxSlot_Padding, 0.f, kPadPx, 0.f, 0.f);
        if (!ui::server_browser_actions::Build(gridWrap, backDonor)) return false;
    } else {
        return false;
    }

    // Back alone at the bottom left, inside the left column: every native window with both puts
    // Back bottom left and its actions bottom right (Settings is Back | Reset all, Apply; the save
    // browser has Back alone). Inside the column rather than in a full-width footer, so the body is
    // the only thing between the title and the window's bottom edge and the status pane's Fill slot
    // runs all the way down with no dead band beside Back.
    if (void* footRow = Spawn(L"HorizontalBox", leftCol)) {
        // Sentence case: VOTV uppercases no button label anywhere.
        g_backBtn = BuildButton(footRow, backDonor, coop::text::FromUtf8Lossy(l10n::Tc("server_browser", "Back")).c_str(),
                                ui::native_screen::kBtnFontPx);
        if (!g_backBtn) return false;
        NS::SetHSlot(NS::SlotOf(g_backBtn), 0.f, NS::kLeft, kCenter);
        if (void* s = NS::AddVFill(leftCol, footRow, 0.f, NS::kLeft, kBottom))
            NS::SetSlotPadding(s, P::off::UVerticalBoxSlot_Padding, 0.f, kPadPx, 0.f, 0.f);
    }

    // The panes' fixed width: a SizeBox in an Automatic slot, so the list's Fill takes the rest.
    void* rightBox = Spawn(L"SizeBox", body);
    void* rightCol = rightBox ? Spawn(L"VerticalBox", rightBox) : nullptr;
    if (!rightBox || !rightCol) return false;
    U::SetSizeBoxWidth(rightBox, kPanelsW);
    U::SetContent(rightBox, rightCol);
    NS::AddHFill(body, rightBox, 0.f, kFill, kFill);
    if (!panels::BuildDetails(rightCol)) return false;
    // Connect sits directly under the panel that describes what it joins; the other actions stay in
    // the grid beneath the list, since they do not depend on the chosen row.
    if (!ui::server_browser_actions::BuildConnect(rightCol, backDonor)) return false;
    if (!panels::BuildStatus(rightCol)) return false;

    g_grip = BuildGrip(shell.frame);   // cosmetic if absent: the window keeps its size
    ApplyWindowSize();

    g_root  = root;
    // The list goes to its owner, which also drops every row identity from the menu instance that
    // just died.
    rows::Attach(list);

    // Attached now, not at the first Show: nothing else references this tree, and attached lazily
    // it was an unreferenced UObject graph between the build and the player's first click, which
    // the garbage collector took; AddChild then returned null on a dead object, and before the
    // index was proven that switched to one of the game's own screens (MULTIPLAYER opened VOTV's
    // Stats panel), after it the button went dead. No lab run saw it because every scenario
    // auto-opens on the build tick. A switcher child is reachable from the menu, the reference
    // actually wanted; AddToRoot is the wrong tool.
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
    // The index was proven at the build; had that failed, g_root was cleared and this is not
    // reached.
    g_priorIndex = NS::SafePriorIndex(U::SwitcherIndex(g_switcher), g_ourIndex, g_priorIndex);
    // The screen stays attached for the menu's life: a switcher renders only its active child, so
    // an inactive child costs nothing, and only the index moves.
    U::SwitcherSetIndex(g_switcher, g_ourIndex);
    g_shown = true;
    g_escPrimed = false;   // re-prime: an ESC held while the screen opens must not close it
    g_lmbPrimed = false;   // ...and the same for the release that OPENED us
    rows::OnShown();       // ...and the hover, for the same reason: nothing else re-asks
    ui::server_browser_tabs::Sync();
    SyncRows();
    g_fit.Rearm();         // a fresh showing lays out again: measured on the next tick
    UE_LOGI("server_browser_native: shown (index %d -> %d)", g_priorIndex, g_ourIndex);
}

void Hide(const char* why) {
    if (!g_shown) return;
    g_shown = false;
    // Restored only if the index is still ours: the game's own sibling screens write this field to
    // navigate, and stomping a navigation the player just made would be worse than leaving it.
    const int32_t now = U::SwitcherIndex(g_switcher);
    // Restoring the index is the whole hand-back: whichever window owned it observes its own index
    // return and revives on its next tick. That reconcile lives in the windows, symmetrically with
    // the one that closes them, so this side need not know whom it displaced.
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
    // The hide touches the engine, so it runs on the game thread from OnMenuTick; this records the
    // request. Close is called from the ImGui picker and the harness, neither on the game thread.
    g_wantClose.store(true, std::memory_order_relaxed);
}

void CloseNow() {
    // Enforced: it reaches ProcessEvent through SwitcherSetIndex, so off the game thread it
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

// The three list questions are the list's; this screen forwards them so a caller holding this
// header need not learn a second one.
int HoveredRow() { return rows::HoveredRow(); }
const char* SelectedRowId() { return rows::SelectedId(); }
bool SelectedRow(coop::net::lobby::LobbyRow& out, std::string* master) {
    return rows::Selected(out, master);
}

// The status pane owns the notice line; this stays as the entry point the actions call.
void SetNotice(const char* text) { panels::SetNotice(text); }

void LogRowHitDiagnostics(int32_t i) { rows::LogRowHitDiagnostics(i); }


void OnMenuTick(void* menu, void* switcher) {
    if (!Armed() || !menu || !switcher) return;
    g_switcher = switcher;

    // A new menu instance: the old widgets died with it, so rebuild.
    if (menu != g_menu) {
        g_menu = menu;
        g_root = nullptr;
        g_backBtn = nullptr; g_scrimW = nullptr; g_frame = nullptr;
        g_listBox = nullptr; g_grip = nullptr; g_dragging = false;
        ui::server_browser_actions::Forget();
        ui::server_browser_tabs::Forget();
        panels::Forget();
        g_ourIndex = -1; g_shown = false; g_buildAttempts = 0; g_toldTheUser = false;
        rows::Attach(nullptr);   // the panel died with the menu; drop it and the row ids
        // The frame donor is a UImage owned by the old menu's ui_settings; kept across the rebuild,
        // CloneStyle would copy 0x88 bytes out of a destroyed widget.
        NS::ForgetBorderDonor();
    }
    if (!g_root) {
        // The attempt is counted here: BuildScreen has a dozen other failing returns (the AddChild
        // failure among them, which clears g_root to force a rebuild), and counted only in its
        // missing-donor guard they retried at menu-tick rate with no backoff and never armed the
        // dialog.
        ++g_buildAttempts;
        if (!BuildScreen(switcher)) return;
        if (AutoOpenArmed()) {
            // The autoopen does not open on the build tick: opening in the same tick took a path no
            // player can take and hid the lazily-attached tree's collection for days, since opening
            // immediately left no gap for GC. The dev path walks the same shape a person does:
            // build, force a collection, let ticks pass, then open.
            UE_LOGW("server_browser_native: [dev] browser_autoopen=1 -- forcing a GC and "
                    "opening in %d ticks, so the lab walks the same build-then-click gap a "
                    "player does", kAutoOpenDelayTicks);
            E::ForceGarbageCollection();
            g_autoOpenIn = kAutoOpenDelayTicks;
        }
    }
    if (g_autoOpenIn > 0 && --g_autoOpenIn == 0) {
        // Through the surface, as the MULTIPLAYER button opens it: the requests that ride an open
        // (the update check, the thanks list) are then part of the lab's run too.
        ui::server_browser_surface::Open();
        selftest::Arm();
    }
    g_menu = menu;

    // The deferred intent is consumed here, inside a main-menu tick: a first-hand observation that
    // the menu is up, stronger than any memoised world reading, and it cannot fire over gameplay.
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

    // The self-check ticks whether or not the screen is shown: its last phases re-open the screen
    // after ESC closed it, and below the shown gate it would stop the moment its own ESC phase
    // succeeded.
    selftest::Tick(g_scrimW, rows::Panel(), g_backBtn);

    // The native-text probe, dev-gated and latched; it needs the browser's own panel, the tree
    // whose behaviour is in question.
    coop::dev::native_text_probe::Tick(rows::Panel());



    // Reconcile in both directions: a sibling screen (or ESC reaching a stale widgetEnter, which
    // the game clears only on its own ESC path) can move ActiveWidgetIndex off ours, and then we
    // were closed, whoever did it; and if it comes back to ours we are on screen again, which is
    // how the input screens hand the browser back.
    const bool indexIsOurs = g_root && g_ourIndex >= 0 &&
                             NS::ActiveIndex() == g_ourIndex;
    if (g_shown && !indexIsOurs) {
        g_shown = false;
        UE_LOGI("server_browser_native: the switcher moved off our index -- treating as closed");
        return;
    }
    if (!g_shown) {
        if (!indexIsOurs) return;
        g_shown = true;
        g_escPrimed = false;   // the screen just became live -- see either hosting window's
        g_lmbPrimed = false;   // BecameLive for why a revive owes these two
        rows::OnShown();
        ui::server_browser_tabs::Sync();
        g_fit.Rearm();
        UE_LOGI("server_browser_native: live again (the switcher index returned to ours)");
    }

    // ESC closes the screen. The game's own ESC handler is a no-op at our index
    // (ui_menu_C::OnKeyDown casts widgetEnter and tests ActiveWidgetIndex == 0, and both fail
    // here), which is fine for ui_saveSlots with its button_back and stranded the player here.
    // Polled in this observer rather than in the WndProc detour: one GetAsyncKeyState per menu
    // tick, no edit to the input path. The key is not swallowed; the game's handler runs too, and
    // the reconcile above sees a navigation away.
    {
        const bool esc = (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if (!g_escPrimed) { g_escPrimed = true; g_prevEsc = esc; }
        const bool pressEdge = esc && !g_prevEsc;
        g_prevEsc = esc;
        // A focused text field owns Escape: GetAsyncKeyState reads the physical key, so consuming
        // WM_KEYDOWN in the detour would still leave this edge firing, and one press would blur the
        // field and close the screen. The field's handler turns Escape into "leave the field".
        if (pressEdge && ui::native_text_field::AnyFocused()) return;
        if (pressEdge) {
            Hide("ESC");
            return;
        }
    }

    // The chrome click on the LBUTTON release edge, as the MULTIPLAYER inject does: the buttons are
    // real UButtons, so the down edge drives Slate's Pressed visual, and closing on it would pull
    // the screen out from under a button that never saw its release. IsHovered is a UFunction and
    // is called only on the release edge.
    {
        const bool down = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (!g_lmbPrimed) { g_lmbPrimed = true; g_prevLmb = down; }
        const bool releaseEdge = !down && g_prevLmb;
        const bool pressEdge   = down && !g_prevLmb;
        g_prevLmb = down;
        // A drag of the grip owns the button from its press to its release: the release is not a
        // click on whatever the cursor ends over.
        if (g_dragging) {
            if (down) {
                UpdateDrag();
            } else {
                g_dragging = false;
                SaveWindowSize();
            }
            return;
        }
        if (pressEdge && g_grip && ui::input_focus::IsOurWindowForeground()) {
            long cx = 0, cy = 0;
            if (NS::CursorInWidgetSpace(cx, cy) && NS::WidgetContains(g_grip, cx, cy)) {
                StartDrag(cx, cy);
                if (g_dragging) return;
            }
        }
        if (releaseEdge && ui::input_focus::IsOurWindowForeground()) {
            // IsHovered is right here: a real UButton answers it, and a geometry test on the same
            // button once turned a passing close into a failing one.
            if (g_backBtn  && E::WidgetIsHovered(g_backBtn))  { Hide("BACK"); return; }
            // The action bar before the rows: its buttons sit outside the list, and returning here
            // keeps a click on CONNECT from also reading as a click on what is behind it.
            if (ui::server_browser_actions::OnReleaseEdge()) return;
            // A tab: another master's list is fetched now, not at the next 5 s tick, and the
            // emptied list and the panes repaint this tick, so the switch is visible at once.
            bool switched = false;
            if (ui::server_browser_tabs::OnReleaseEdge(switched)) {
                if (switched) {
                    g_lastRefreshMs = ::GetTickCount64();
                    sm::Refresh();
                    SyncRows();
                    char notice[256];
                    l10n::Fmt(notice, sizeof(notice), l10n::T("Showing the %s server list."),
                              coop::net::master_slots::Selected().label.c_str());
                    panels::SetNotice(notice);
                }
                return;
            }
            // A click on a hovered row selects it; the row is known from the hover pass, so this
            // costs no dispatch, and a handled click returns like the two above. The details panel
            // repaints now rather than at the 1 Hz cadence: a pane that fills in a second later
            // reads as a click that did not register.
            if (rows::ClickSelect()) { panels::Sync(true); return; }
        }
    }

    rows::UpdateHover();
    ui::server_browser_tabs::UpdateHover();
    g_fit.Tick(g_frame, g_backBtn, "master tabs, list, actions, Back");   // after a showing only

    // Fetch on a timer, paint on an arrival: with paint coupled to the fetch tick a lobby that
    // arrived early was not drawn until the next fetch, and REFRESH showed "Refreshing..." over an
    // unchanged list. CopyRows returns a generation per completed fetch; the sibling window uses
    // the same shape.
    const uint64_t now = ::GetTickCount64();
    if (now - g_lastRefreshMs >= kRefreshMs) {
        g_lastRefreshMs = now;
        sm::Refresh();
    }
    if (sm::RowsGeneration() != rows::PaintedGeneration()) SyncRows();

    // The panes carry two seconds counters, so they repaint on their own 1 Hz cadence between
    // fetches; a line writes only when its text changed.
    panels::Sync(false);
}

}  // namespace ui::server_browser_native
