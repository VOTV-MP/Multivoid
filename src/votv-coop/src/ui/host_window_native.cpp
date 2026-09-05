// ui/host_window_native.cpp -- the hosting window, step one: the world (New game or a save) and
// the connection mode, handed to step two (host_session_settings) on Next. Built on
// ui/native_screen's kit and shaped after ui/server_browser_native, its sibling in the same
// switcher; a measured construction fact lives in the kit's header, not here. See
// ui/host_window_native.h.

#include "ui/host_window_native.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/session/session_manager.h"
#include "ui/host_session_settings.h"   // step two, what Next opens
#include "ui/input_focus.h"
#include "ui/native_screen.h"
#include "ui/server_browser_native.h"   // CloseNow, the sibling hand-over
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/save_browser.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

namespace ui::host_window_native {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace U  = ue_wrap::umg;
namespace P  = ue_wrap::profile;
namespace NS = ui::native_screen;
namespace sm = coop::session_manager;
namespace sb = ue_wrap::save_browser;

using ue_wrap::FLinearColor;

// Layout. 980 matches the browser, and at 900 the connection descriptions clipped mid-word, which
// reads as a rendering fault rather than a long sentence.
constexpr float kWindowW  = 980.f;
constexpr float kWindowH  = 640.f;
constexpr float kRowH     = 56.f;
constexpr float kBorderPx = 2.f;
constexpr float kPadPx    = 6.f;
// The list height is explicit, not the VerticalBox's leftover slack, and it is a minimum, not a
// budget: with every child Automatic the column asked for more than the window had and the footer
// was arranged past the bottom edge, Back and Host hanging outside the frame. The list's slot is
// Fill (its AddVFill below), so it absorbs the column's slack, this value is what the SizeBox asks
// for, and no sibling can be arranged off the frame at any font metric; the browser's column has
// always had that shape.
constexpr float kListH    = 240.f;
// Bounds the whole sync loop, not only the display: a player with more saves sees the newest
// (save_browser sorts by last played), and the cap is stated.
constexpr int   kMaxSaveRows = 24;

const FLinearColor kPanel  = NS::Panel();
const FLinearColor kRowBg  = NS::RowBg();
const FLinearColor kRowSel = NS::RowSel();
const FLinearColor kText   = NS::Text();
const FLinearColor kAccent = NS::Accent();
const FLinearColor kHover  = NS::Hover();
const FLinearColor kDim    = NS::Dim();

// The connection modes, the product wording fixed once; each line says what the choice costs the
// player.
struct ConnMode { const wchar_t* title; const wchar_t* detail; };
// Each description fits its cell, a constraint on the writing: the row is a fixed-width
// two-column line, and a sentence longer than about 50 characters clips mid-word. Two modes, not
// three: LAN ONLY was never a third transport, it called the same StartLanDirect and bound the
// same all-interfaces socket, and what made it look separate was an accept filter doing the
// router's job (deleted) plus never listing, which is the server-list selector in step two (see
// coop/session/host_mode.h). The names say what the player must do and what it costs them.
constexpr ConnMode kConnModes[2] = {
    {L"AUTOMATIC  (recommended)",
     L"We introduce you. Nothing to set up."},
    {L"DIRECT  (you forward a port)",
     L"Friends reach your PC. No server involved."},
};

// State, game thread only unless marked.
void* g_menu     = nullptr;
void* g_switcher = nullptr;
void* g_root     = nullptr;
void* g_scrimW   = nullptr;
void* g_list     = nullptr;   // UScrollBox of save rows
void* g_status   = nullptr;
// The status string last written to g_status, at module level and reset with the widget on the
// menu-instance edge (see the writer in OnMenuTick).
std::string g_lastStatus;
void* g_backBtn  = nullptr;
void* g_hostBtn  = nullptr;
// Sized from the table, never spelled again: as a literal beside a literal-sized table, every
// loop was a separate place to forget when the table shrank, and an over-run loop reads a null
// and silently draws nothing.
constexpr int kConnCount = static_cast<int>(sizeof(kConnModes) / sizeof(kConnModes[0]));
void* g_connRow[kConnCount]   = {};   // the clickable background images
void* g_connLabel[kConnCount] = {};

int32_t g_ourIndex   = -1;
int32_t g_priorIndex = -1;
bool    g_shown      = false;
int     g_buildAttempts = 0;

// The chosen world by slot name; empty is New game, the default, since a fresh world is the only
// choice that always exists. Not an index: the list is sorted newest-first by last played and
// re-scanned on every Show, so an index chosen before a host, a quit and a reopen names a
// different world, and a wrong slot is a wrong save. The browser keys on lobbyId for the same
// reason.
std::wstring g_selectedSlot;
int  g_connMode     = 0;
int  g_hoverRow     = -2;   // -2 = nothing hovered; -1 = the New Game row; >=0 = a save
int  g_hoverConn    = -1;
int  g_visibleSaves = 0;         // rows actually SHOWN, never g_saveRows.size()
NS::HoverTracker g_hover;        // pointer + scroll + settling, shared shape with the browser

bool  g_prevLmb   = false;
bool  g_lmbPrimed = false;
bool  g_prevEsc   = false;
bool  g_escPrimed = false;

std::vector<sb::SaveInfo> g_saves;
uint64_t g_savesRev = 0;

// Cross-thread open and close intents, as the browser's.
std::atomic<uint64_t> g_wantOpenMs{0};
std::atomic<bool>     g_wantClose{false};
constexpr uint64_t kIntentTtlMs = 20000;

// Helpers.

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Where the chosen world currently sits, or -1 for New game; resolved by name on every read, so a
// re-sort under an open window cannot move the selection to a different save.
int SlotIndex() {
    if (g_selectedSlot.empty()) return -1;
    for (size_t i = 0; i < g_saves.size(); ++i)
        if (g_saves[i].slot == g_selectedSlot) return static_cast<int>(i);
    return -1;
}

// One row: SizeBox, Overlay, an Image (the hit target and the selection fill) and a HorizontalBox
// of text. No UButton: a bare UImage answers the geometry hit test, and a UButton would add a
// press visual to suppress.
struct Row { void* box; void* bg; void* a; void* b; void* c; };

Row BuildRow(void* parent, float wA, float wB, float wC) {
    Row r{};
    r.box = NS::Spawn(L"SizeBox", parent);
    if (!r.box) return r;
    U::SetSizeBoxHeight(r.box, kRowH);
    void* ovl = NS::Spawn(L"Overlay", r.box);
    if (!ovl) return Row{};
    r.bg = NS::Spawn(L"Image", ovl);
    if (!r.bg) return Row{};
    U::SetImageTintRaw(r.bg, kRowBg);
    E::SetWidgetVisibility(r.bg, 0);   // Visible: it is the hit target
    if (void* s = U::AddChild(ovl, r.bg))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                        NS::kFill, NS::kFill);
    if (void* hb = NS::Spawn(L"HorizontalBox", ovl)) {
        r.a = NS::AddText(hb, L"", 18, kText, NS::kJustLeft, wA);
        if (wB > 0.f) r.b = NS::AddText(hb, L"", 15, kDim, NS::kJustLeft, wB);
        if (wC > 0.f) r.c = NS::AddText(hb, L"", 15, kDim, NS::kJustLeft, wC);
        if (void* s = U::AddChild(ovl, hb)) {
            U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                            NS::kFill, NS::kCenter);
            auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                                 P::off::UOverlaySlot_Padding);
            pad[0] = 12.f; pad[1] = 0.f; pad[2] = 12.f; pad[3] = 0.f;
        }
    }
    U::SetContent(r.box, ovl);
    // No half-built row: SyncSaves pushes what this returns and indexes g_saves and g_list by the
    // same integer, so a failed row that was still pushed would slide every later index by one and
    // a click would select a different world.
    if (!r.box || !r.bg) return Row{};
    U::AddChild(parent, r.box);
    return r;
}

// The save rows, parallel to g_saves. Index -1 is the New Game row, a permanent child that exists
// even when the scan found nothing, the first-run case.
Row              g_newGameRow{};
std::vector<Row> g_saveRows;

void SetText(void* block, const std::wstring& t, const FLinearColor& col) {
    if (!block) return;
    E::SetWidgetText(block, t.c_str());
    E::SetTextBlockColorDispatch(block, col);
}

// A runtime repaint, so every write dispatches: the raw variants write a property UMG already
// baked into the Slate widget at attach and change nothing on screen (this window's hover
// highlight never drew that way); Raw is correct at build time only.
void PaintRow(const Row& r, bool selected, bool hovered) {
    if (!r.bg) return;
    U::SetImageTint(r.bg, selected ? kRowSel : kRowBg);
    // Hover is a text colour and selection a fill, two channels applied independently.
    const FLinearColor main = hovered ? kHover : kText;
    const FLinearColor sub  = hovered ? kHover : kDim;
    E::SetTextBlockColorDispatch(r.a, main);
    E::SetTextBlockColorDispatch(r.b, sub);
    E::SetTextBlockColorDispatch(r.c, sub);
}

void RepaintAll() {
    PaintRow(g_newGameRow, g_selectedSlot.empty(), g_hoverRow == -1);
    for (size_t i = 0; i < g_saveRows.size(); ++i)
        PaintRow(g_saveRows[i], SlotIndex() == static_cast<int>(i),
                 g_hoverRow == static_cast<int>(i));
    for (int i = 0; i < kConnCount; ++i) {
        if (!g_connRow[i]) continue;
        U::SetImageTint(g_connRow[i], g_connMode == i ? kRowSel : kRowBg);
        E::SetTextBlockColorDispatch(g_connLabel[i], g_hoverConn == i ? kHover : kText);
    }
}

void SetStatus(const std::wstring& t) { SetText(g_status, t, kText); }

}  // namespace

namespace {

bool BuildScreen(void* switcher) {
    void* saveSlots = NS::SwitcherChild(switcher, L"ui_saveSlots_C");
    void* backDonor = NS::DonorField(saveSlots, L"button_back");
    if (!backDonor) {
        // Fail closed and retry: a missing donor means the build's layout moved. The browser owns
        // the player-facing alarm for that; this screen is reached through it, and a second dialog
        // would stack.
        if (++g_buildAttempts == 15)
            UE_LOGE("host_window_native: ui_saveSlots_C.button_back absent after %d "
                    "attempts -- NOT building", g_buildAttempts);
        return false;
    }

    // The shell (the switcher child, the widget tree, the scrim, the centred framed window, the
    // title strip) from the shared kit. No X: the exits are Back and ESC.
    NS::WindowShell shell;
    if (!NS::BuildWindowShell(switcher, kWindowW, kWindowH,
                              L"Multivoid  -  Host Game", shell))
        return false;
    void* root = shell.root;
    void* col  = shell.column;
    g_scrimW   = shell.scrim;

    NS::AddText(col, L"WORLD", 16, kAccent, NS::kJustLeft, 0.f);
    g_newGameRow = BuildRow(col, 1.f, 0.f, 0.f);
    SetText(g_newGameRow.a, L"New game", kText);

    void* listBox = NS::Spawn(L"SizeBox", col);
    g_list = listBox ? NS::Spawn(L"ScrollBox", listBox) : nullptr;
    if (!listBox || !g_list) return false;
    U::SetSizeBoxHeight(listBox, kListH);
    U::SetContent(listBox, g_list);
    // The one Fill child in this column, which makes the window structurally safe rather than
    // arithmetically lucky: with every child Automatic the column was a fixed stack, and the moment
    // a desired size grew past the budget the footer was arranged outside the window and its
    // buttons became unclickable. With the list absorbing the slack, kListH is a desired minimum.
    NS::AddVFill(col, listBox, 1.f, NS::kFill, NS::kFill);

    NS::AddText(col, L"CONNECTION", 16, kAccent, NS::kJustLeft, 0.f);
    for (int i = 0; i < kConnCount; ++i) {
        Row r = BuildRow(col, 0.42f, 0.58f, 0.f);
        g_connRow[i]   = r.bg;
        g_connLabel[i] = r.a;
        SetText(r.a, kConnModes[i].title,  kText);
        SetText(r.b, kConnModes[i].detail, kDim);
    }

    // The footer: Back at the left, Next at the right, the status between, and no bordered strip
    // around them (a framed bar level with the window's own frame read as a second window edge;
    // VOTV frames content, never a row of buttons). Bottom left is the way out and bottom right the
    // commit in every native window that has both.
    if (void* footRow = NS::Spawn(L"HorizontalBox", col)) {
        // Sentence case: VOTV uppercases no button label anywhere.
        g_backBtn = NS::BuildButton(footRow, backDonor, L"Back", NS::kBtnFontPx);
        g_status  = NS::AddText(footRow, L"", 16, kText, NS::kJustCenter, 1.f);
        // "Next", not "Host": this window no longer hosts, and a label promising the last step
        // while a second one follows is a lie the player notices at once.
        g_hostBtn = NS::BuildButton(footRow, backDonor, L"Next", NS::kBtnFontPx);
        if (!g_backBtn || !g_hostBtn) return false;
        // The status text carries all the fill weight, so it takes the slack and pushes the two
        // buttons to the ends.
        NS::SetHSlot(NS::SlotOf(g_backBtn), 0.f, NS::kLeft,  NS::kCenter);
        NS::SetHSlot(NS::SlotOf(g_hostBtn), 0.f, NS::kRight, NS::kCenter);
        if (void* s = NS::AddVFill(col, footRow, 0.f, NS::kFill, NS::kBottom))
            NS::SetSlotPadding(s, P::off::UVerticalBoxSlot_Padding, 0.f, kPadPx, 0.f, 0.f);
    }

    g_root = root;

    // Attached now, not at the first Show: nothing else references this tree, and attached lazily
    // it was an unreferenced UObject graph between the build and the player's first click, which
    // the garbage collector took (AddChild then returned null on a dead object; before the index
    // was proven that switched to one of the game's own screens, after it the button went dead). No
    // lab run saw it because every scenario auto-opens on the build tick. A switcher child is
    // reachable from the menu, the reference actually wanted; AddToRoot is the wrong tool.
    {
        void* slot = U::AddChild(g_switcher, g_root);
        g_ourIndex = U::IndexOfChild(g_switcher, g_root);
        if (g_ourIndex < 0) {
            UE_LOGE("host_window_native: built the hosting window but could NOT place it in the menu switcher "
                    "(AddChild slot=%p, GetChildIndex=-1). The screen cannot be shown this "
                    "menu; it will be rebuilt on the next one.", slot);
            ue_wrap::log::Flush();
            g_root = nullptr;   // force a rebuild rather than keep an unreachable tree
            return false;
        }
    }
    UE_LOGI("host_window_native: screen built (root=%p list=%p) after %d attempt(s)",
            root, g_list, g_buildAttempts + 1);
    return true;
}

void SyncSaves() {
    const uint64_t rev = sb::CopySaves(g_saves);
    if (rev == g_savesRev && !g_saveRows.empty()) return;
    g_savesRev = rev;

    const int want = static_cast<int>(g_saves.size()) > kMaxSaveRows
                         ? kMaxSaveRows : static_cast<int>(g_saves.size());
    // What the hover walk may consider: the row vector is a high-water mark (rows are grown and
    // collapsed, never removed) and a collapsed widget keeps the rect it last painted with, so the
    // vector size let a click under the live rows hover a row not on screen.
    g_visibleSaves = want;
    while (static_cast<int>(g_saveRows.size()) < want)
        g_saveRows.push_back(BuildRow(g_list, 0.5f, 0.25f, 0.25f));
    for (int i = 0; i < static_cast<int>(g_saveRows.size()); ++i) {
        const bool live = i < want;
        E::SetWidgetVisibility(g_saveRows[i].box, live ? 0 : 1);   // Visible / Collapsed
        if (!live) continue;
        const sb::SaveInfo& s = g_saves[static_cast<size_t>(i)];
        SetText(g_saveRows[i].a, s.displayName.empty() ? s.slot : s.displayName, kText);
        SetText(g_saveRows[i].b, s.modeLabel, kDim);
        SetText(g_saveRows[i].c, L"day " + std::to_wstring(s.day), kDim);
    }
    // Nothing to repair: the selection is a slot name, so a re-sort, a shrink and a rescan leave it
    // on the same world, or on none if that save is gone.

    RepaintAll();
}

void UpdateHover() {
    // The tracker, not a cursor delta of our own: a pointer-only gate over a scrollable list let a
    // wheel turn slide a different world under a still pointer while the stored index, which is
    // what Next loads, stayed put; the browser had the same gate and its fix was not carried
    // across. The tracker also supplies the settling pass Slate's one-tick-late hover needs.
    if (!g_hover.Poll(g_list, g_visibleSaves)) return;

    const int prevRow = g_hoverRow, prevConn = g_hoverConn;
    g_hoverRow  = -2;
    g_hoverConn = -1;
    // New Game and the connection rows sit outside the ScrollBox, and the save rows inside it,
    // where IsHovered reads 0 on a Visible row image whose rect contains the cursor; left to Slate,
    // the world list could not be clicked at all. One cursor resolve for the whole sweep:
    // CursorOverWidget re-resolves per widget, and its expensive half is an uncached
    // FindObjectByClass walk, so the new-game row plus each connection row cost one walk each per
    // evaluated tick.
    long hx = 0, hy = 0;
    // Falls through to the repaint on failure: the hover state was already cleared above, and an
    // early return left the last highlighted row lit permanently.
    if (NS::CursorInWidgetSpace(hx, hy)) {
        if (g_newGameRow.bg && NS::WidgetContains(g_newGameRow.bg, hx, hy)) g_hoverRow = -1;
        if (g_hoverRow == -2 && g_hover.Index() >= 0) g_hoverRow = g_hover.Index();
        for (int i = 0; i < kConnCount; ++i)
            if (g_connRow[i] && NS::WidgetContains(g_connRow[i], hx, hy)) { g_hoverConn = i; break; }
    }
    if (g_hoverRow != prevRow || g_hoverConn != prevConn) RepaintAll();
}

// Next hands the two choices made here to step two, which owns the host call: a hosting path here
// as well would be two host actions.
void DoNext() {
    sm::SaveChoice c;
    const int sel = SlotIndex();
    if (sel < 0) {
        c.newGame = true;
        // A literal nobody typed (SaveChoice::nameIsDerived): without the flag the second New Game
        // hosted from here died on "slot already exists".
        c.newName       = "Coop";
        c.nameIsDerived = true;
        c.mode    = 0;   // enum_gamemode story
    } else {
        c.newGame = false;
        c.slot    = Narrow(g_saves[static_cast<size_t>(sel)].slot);
    }
    // The name, derived rather than typed: the browser lists servers by name and "<nick>'s game"
    // tells another player who is hosting. Resolved at the moment the player commits to a world, so
    // the two halves of one decision travel together.
    const std::string name = sm::Nickname().empty() ? "Multivoid game"
                                                    : sm::Nickname() + "'s game";
    UE_LOGI("host_window_native: NEXT -- world=%s conn=%d name='%s'",
            c.newGame ? "<new game>" : c.slot.c_str(), g_connMode, name.c_str());
    ui::host_session_settings::Open(c, name, g_connMode);
}

void Show();
void Hide(const char* why);

void PollChrome() {
    if (!ui::input_focus::IsOurWindowForeground()) return;

    const bool esc = (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (!g_escPrimed) { g_escPrimed = !esc; g_prevEsc = esc; }
    else if (g_prevEsc && !esc) { g_prevEsc = esc; Hide("ESC"); return; }
    else g_prevEsc = esc;

    const bool lmb = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (!g_lmbPrimed) { g_lmbPrimed = !lmb; g_prevLmb = lmb; return; }
    const bool released = g_prevLmb && !lmb;
    g_prevLmb = lmb;
    if (!released) return;

    // Two mechanisms for two kinds of widget: a real UButton answers IsHovered, a hand-built UImage
    // does not, and unifying both on geometry turned a passing close into a failing one.
    if (g_backBtn  && E::WidgetIsHovered(g_backBtn))  { Hide("BACK"); return; }
    if (g_hostBtn  && E::WidgetIsHovered(g_hostBtn))  { DoNext(); return; }
    for (int i = 0; i < kConnCount; ++i)
        if (g_connRow[i] && NS::CursorOverWidget(g_connRow[i])) { g_connMode = i; RepaintAll(); return; }
    if (g_newGameRow.bg && NS::CursorOverWidget(g_newGameRow.bg)) {
        g_selectedSlot.clear(); RepaintAll(); return;
    }
    // The row under the cursor is known from the hover pass (geometry, for the reason recorded
    // there); re-deriving it here would be a second implementation that could disagree. Bounded by
    // the saves, not by the row widgets: a hover index outside them is a collapsed row and selects
    // nothing, so this branch is not the fall-through for every click that hits no control.
    if (g_hoverRow >= 0 && g_hoverRow < static_cast<int>(g_saves.size())) {
        g_selectedSlot = g_saves[static_cast<size_t>(g_hoverRow)].slot;
        RepaintAll();
        return;
    }
}

// Everything that must be true the moment the screen becomes live, with one owner because there
// are two ways it happens: Show, and the reconcile that revives it when the switcher index comes
// back. The revive once skipped these: the browser closes on the ESC press edge and this window
// on the release, so one keypress closed the browser, revived this window with a stale primed
// flag, and the release closed this one too. The content resets stay in Show: re-reading the ini
// or clearing the status on a revive would wipe a half-typed password and erase the host-failure
// line this window exists to display.
void BecameLive() {
    g_hover.Reset();
    g_hoverRow  = -2;
    g_hoverConn = -1;
    g_escPrimed = false;
    g_lmbPrimed = false;
}

void Show() {
    if (!g_switcher || !g_root || g_shown) return;
    // The index was proven at the build; had that failed, g_root was cleared and this is not
    // reached.
    g_priorIndex = NS::SafePriorIndex(U::SwitcherIndex(g_switcher), g_ourIndex, g_priorIndex);
    U::SwitcherSetIndex(g_switcher, g_ourIndex);
    g_shown = true;
    // The old hover is forgotten: reopening does not move the pointer, and the click path reads the
    // index, so a click on inert chrome would select the row the cursor was left over minutes ago.
    BecameLive();
    sb::RefreshAsync();          // the list is stale by definition between openings
    SyncSaves();
    // The status is written by the one edge-gated writer in OnMenuTick, on this same tick; a second
    // writer here needed a second cache, and two caches for one widget is what once left the line
    // permanently blank.
    UE_LOGI("host_window_native: shown (index %d -> %d)", g_priorIndex, g_ourIndex);
}

void Hide(const char* why) {
    if (!g_shown) return;
    g_shown = false;
    const int32_t now = U::SwitcherIndex(g_switcher);
    if (now == g_ourIndex && g_priorIndex >= 0) U::SwitcherSetIndex(g_switcher, g_priorIndex);
    UE_LOGI("host_window_native: hidden (%s; index was %d, ours %d)", why, now, g_ourIndex);
}

bool Armed() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::browser_native);
    return s;
}

// The lab reaches this screen without a click, behind a dev flag, the way it reaches the browser.
bool AutoOpenArmed() {
    static const bool s =
        coop::config::ResolveFlag(::coop::config_registry::rows::host_window_autoopen);
    return s && Armed();
}

}  // namespace

void Open()   { g_wantOpenMs.store(::GetTickCount64(), std::memory_order_relaxed); }
void Close()  { g_wantOpenMs.store(0, std::memory_order_relaxed);
                g_wantClose.store(true, std::memory_order_relaxed); }

void CloseNow() {
    // Enforced: this reaches ProcessEvent through SwitcherSetIndex, so off the game thread it
    // degrades to the deferred close rather than touching the engine.
    if (!ue_wrap::game_thread::IsGameThread()) {
        UE_LOGW("host_window_native: CloseNow off the game thread -- deferring instead "
                "(it drives the switcher through ProcessEvent)");
        Close();
        return;
    }
    g_wantOpenMs.store(0, std::memory_order_relaxed);
    g_wantClose.store(false, std::memory_order_relaxed);   // performed here, not deferred
    Hide("replaced by a sibling screen");
}

bool IsOpen() { return g_shown; }

void* BackButton() { return g_backBtn; }
void* NextButton() { return g_hostBtn; }

int   SelectedSave()   { return SlotIndex(); }
int   SaveRowCount()   { return g_visibleSaves; }   // shown, not the row vector's high-water mark
void* SaveListWidget() { return g_list; }

void OnMenuTick(void* menu, void* switcher) {
    if (!Armed() || !menu || !switcher) return;
    g_switcher = switcher;

    if (menu != g_menu) {
        g_menu = menu;
        g_root = nullptr; g_list = nullptr; g_status = nullptr; g_scrimW = nullptr;
        g_backBtn = nullptr; g_hostBtn = nullptr;
        g_newGameRow = Row{};
        g_saveRows.clear();
        for (int i = 0; i < kConnCount; ++i) { g_connRow[i] = nullptr; g_connLabel[i] = nullptr; }
        g_ourIndex = -1; g_shown = false; g_buildAttempts = 0; g_savesRev = 0;
        g_lastStatus.clear();   // the widget it cached is gone with the menu
    }
    if (!g_root) {
        // Backed off once hopeless, like both siblings: a persistent donor failure otherwise re-ran
        // the switcher walk and the donor lookups (an engine call and a wstring per child) every
        // menu tick, at ~117 Hz, on the path a version migration lands on.
        if (g_buildAttempts >= 15) {
            static uint64_t sNextTryMs = 0;
            const uint64_t nowTry = ::GetTickCount64();
            if (nowTry < sNextTryMs) return;
            sNextTryMs = nowTry + 1000;
        }
        if (!BuildScreen(switcher)) {
            // A failed build counts whatever failed: counted only on the missing-donor path, the
            // gate above was inert for a shell that would not spawn or a null list, and the tick
            // rebuilt at ~117 Hz forever, spawning a window's worth of UObjects each time.
            ++g_buildAttempts;
            // Said once: the browser's HOST button closes the browser synchronously and then asks
            // for this window, so a build that keeps failing leaves the player on the main menu
            // with no window and no browser, and Open returns void, so the log is the only place
            // the truth can go. Latched: unlatched it ran every menu tick for the intent's 20 s
            // TTL, each line a synchronous flush.
            static bool sSaidSo = false;
            if (!sSaidSo && g_wantOpenMs.load(std::memory_order_relaxed)) {
                sSaidSo = true;
                UE_LOGE("host_window_native: a HOST request is pending but the screen will "
                        "not build (attempt %d) -- the player clicked HOST, the browser "
                        "closed, and nothing opened", g_buildAttempts);
            }
            return;
        }
        if (AutoOpenArmed()) {
            UE_LOGW("host_window_native: [dev] host_window_autoopen=1 -- showing without a click");
            // The same hand-over the real door performs: with both dev flags armed the browser
            // auto-opens too, and opening on top of it would record the browser's index as the one
            // to restore. A test door that differs from the player's door is how a lab result lies.
            ui::server_browser_native::CloseNow();
            Open();
        }
    }
    g_menu = menu;

    if (g_wantClose.exchange(false, std::memory_order_relaxed)) Hide("requested");
    const uint64_t want = g_wantOpenMs.load(std::memory_order_relaxed);
    if (want) {
        const uint64_t age = ::GetTickCount64() - want;
        g_wantOpenMs.store(0, std::memory_order_relaxed);
        if (age <= kIntentTtlMs) Show();
        else
            // Logged, as the browser's expiry is: a HOST click arriving with no menu tick coming
            // otherwise vanished without trace.
            UE_LOGW("host_window_native: a HOST request expired unconsumed after %llu ms "
                    "(ttl %llu) -- no main-menu tick arrived to show the window",
                    static_cast<unsigned long long>(age),
                    static_cast<unsigned long long>(kIntentTtlMs));
    }
    // Reconcile against the live index in both directions: a sibling navigating away is observed,
    // and so is one navigating back. Losing the screen by observation but regaining it only by
    // being told let a caller hand the switcher back by writing the index and leave this window
    // drawn but answering nothing.
    const bool indexIsOurs = g_root && g_ourIndex >= 0 &&
                             NS::ActiveIndex() == g_ourIndex;
    if (g_shown && !indexIsOurs) { g_shown = false; return; }
    if (!g_shown) {
        if (!indexIsOurs) return;
        g_shown = true;
        BecameLive();
        UE_LOGI("host_window_native: live again (the switcher index returned to ours)");
    }

    SyncSaves();
    UpdateHover();
    PollChrome();
    // The host status is authored on a worker thread and outlives this window's closing, so it is
    // shown here where the action was taken. Edge-gated: an unconditional write cost two
    // dispatches, a wstring and an FText per frame for a string that changes when a host attempt
    // finishes.
    if (g_status) {
        // Module level and cleared with the widget: as a function-local static with process
        // lifetime while g_status is rebuilt empty on every menu instance, a status rendered on one
        // menu left the line blank on the next until the status happened to change, and the reason
        // a session died was gone after a quit and a reopen.
        std::string cur = sm::HostStatus();
        if (cur != g_lastStatus) { g_lastStatus = cur; SetStatus(Widen(cur)); }
    }
}

}  // namespace ui::host_window_native
