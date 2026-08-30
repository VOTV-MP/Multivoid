// ui/host_window_native.cpp -- see ui/host_window_native.h for WHY.
//
// Built on ui/native_screen's kit and shaped after ui/server_browser_native, deliberately:
// the two are siblings in the same switcher and a player should not be able to tell they
// were written on different days. Where a construction fact is measured, it lives in the
// kit's header, not repeated here.

#include "ui/host_window_native.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/session/session_manager.h"
#include "ui/input_focus.h"
#include "ui/native_screen.h"
#include "ui/server_browser_native.h"   // CloseNow -- the sibling hand-over
#include "ue_wrap/core/call.h"
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

// ---- layout ------------------------------------------------------------------------
constexpr float kWindowW  = 900.f;
constexpr float kWindowH  = 640.f;
constexpr float kRowH     = 56.f;
constexpr float kBorderPx = 2.f;
constexpr float kPadPx    = 6.f;
// EXPLICIT, not the VerticalBox's leftover slack -- the browser measured (2026-08-26) that
// a Fill slot allots more than the window has left, and the list then overflows UPWARD
// under the header. An override depends on nothing.
constexpr float kListH    = 300.f;
// Bounds the whole sync loop, not just the display. A player with more saves than this
// sees the newest ones (save_browser sorts by last-played), and the cap is stated rather
// than silent.
constexpr int   kMaxSaveRows = 24;

const FLinearColor kPanel  = NS::Panel();
const FLinearColor kRowBg  = NS::RowBg();
const FLinearColor kRowSel = NS::RowSel();
const FLinearColor kText   = NS::Text();
const FLinearColor kAccent = NS::Accent();
const FLinearColor kHover  = NS::Hover();
const FLinearColor kDim    = NS::Dim();

// ---- the three connection modes ----------------------------------------------------
// The wording is the product surface, so it is fixed ONCE here. Each line says what the
// choice costs the player, because "AUTO / DIRECT / LAN" alone asks them to guess.
struct ConnMode { const wchar_t* title; const wchar_t* detail; };
constexpr ConnMode kConnModes[3] = {
    {L"AUTO  (recommended)",
     L"Uses the Multivoid server to introduce you. Works behind almost any router."},
    {L"DIRECT  (port forward)",
     L"Friends connect straight to you. You must forward the port yourself."},
    {L"LAN ONLY",
     L"Never contacts any Multivoid server. Same-network friends connect by your local IP."},
};

// ---- state (GAME THREAD ONLY unless marked) ----------------------------------------
void* g_menu     = nullptr;
void* g_switcher = nullptr;
void* g_root     = nullptr;
void* g_scrimW   = nullptr;
void* g_list     = nullptr;   // UScrollBox of save rows
void* g_status   = nullptr;
void* g_closeBtn = nullptr;
void* g_backBtn  = nullptr;
void* g_hostBtn  = nullptr;
void* g_connRow[3]   = {nullptr, nullptr, nullptr};   // the clickable background images
void* g_connLabel[3] = {nullptr, nullptr, nullptr};

int32_t g_ourIndex   = -1;
int32_t g_priorIndex = -1;
bool    g_shown      = false;
int     g_buildAttempts = 0;

// Selection. -1 is NEW GAME and is the default, because a fresh world is the only choice
// that always exists -- a first-time host may have no saves at all.
// THE CHOSEN WORLD, BY SLOT NAME -- empty means New game.
//
// It was an INDEX, and the list is `stable_sort`ed NEWEST-FIRST by last-played and re-scanned
// on every Show(). So: pick world C (index 2), host it, quit to the menu, reopen -- C is now
// first, index 2 is a different world, the guard `if (idx >= want) idx = -1` does not fire
// because nothing SHRANK, and HOST loads the wrong world with the stale index arriving
// pre-highlighted. The browser learned this as its invariant 1 and keys on `lobbyId`; the
// cost here is higher, because a wrong lobby is a wrong server and a wrong slot is a wrong
// SAVE. Found by the post-ship audit; newly reachable, because until the same day's hit-test
// fix this value could only ever be "none".
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

// Cross-thread open/close intent, same shape as the browser's.
std::atomic<uint64_t> g_wantOpenMs{0};
std::atomic<bool>     g_wantClose{false};
constexpr uint64_t kIntentTtlMs = 20000;

// ---- helpers -----------------------------------------------------------------------

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

// WHERE THE CHOSEN WORLD CURRENTLY SITS, or -1 for none/New game. Resolved by NAME on every
// read, so a re-sort under an open window cannot silently move the selection to a different
// save -- the whole point of keying on the slot instead of the row number.
int SlotIndex() {
    if (g_selectedSlot.empty()) return -1;
    for (size_t i = 0; i < g_saves.size(); ++i)
        if (g_saves[i].slot == g_selectedSlot) return static_cast<int>(i);
    return -1;
}

// One row: SizeBox -> Overlay -> [ Image (the HIT TARGET + the selection fill),
// HorizontalBox of text ]. No UButton, for the reason the browser's rows record: a bare
// UImage answers IsHovered, and a UButton would add a press visual we would suppress.
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
    // BAIL BEFORE PUBLISHING A HALF-BUILT ROW. `SyncSaves` pushes whatever this returns into
    // `g_saveRows` and then indexes `g_saves` and `g_list` by the SAME integer -- so a row
    // that failed to spawn but still got pushed slides every later index by one, and
    // `g_hoverRow` (a g_list index) would select a different world than the one clicked.
    if (!r.box || !r.bg) return Row{};
    U::AddChild(parent, r.box);
    return r;
}

// The save rows we built, parallel to g_saves. Index -1 is the New Game row, which is a
// permanent child rather than a synthesized entry -- it must exist even when the scan
// found nothing, which is exactly the first-run case.
Row              g_newGameRow{};
std::vector<Row> g_saveRows;

void SetText(void* block, const std::wstring& t, const FLinearColor& col) {
    if (!block) return;
    E::SetWidgetText(block, t.c_str());
    E::SetTextBlockColorDispatch(block, col);
}

// RUNTIME repaint, so every write here must DISPATCH. Both raw variants -- the text
// colour and the image tint -- write a property that UMG already baked into the Slate
// widget at attach, so they change nothing on screen; that is why this window's hover
// highlight had never drawn (post-ship audit, 2026-08-30, found via the browser's own
// identical defect). `...Raw` stays correct at BUILD time, before the widget is attached.
void PaintRow(const Row& r, bool selected, bool hovered) {
    if (!r.bg) return;
    U::SetImageTint(r.bg, selected ? kRowSel : kRowBg);
    // Style doc section 4: hover is a TEXT colour and selection is a FILL. Two channels,
    // applied independently -- porting ImGui's HeaderHovered here would look foreign.
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
    for (int i = 0; i < 3; ++i) {
        if (!g_connRow[i]) continue;
        U::SetImageTint(g_connRow[i], g_connMode == i ? kRowSel : kRowBg);
        E::SetTextBlockColorDispatch(g_connLabel[i], g_hoverConn == i ? kHover : kText);
    }
}

void SetStatus(const std::wstring& t) { SetText(g_status, t, kText); }

}  // namespace

// ============================ build ==================================================
namespace {

bool BuildScreen(void* switcher) {
    void* saveSlots = NS::SwitcherChild(switcher, L"ui_saveSlots_C");
    void* backDonor = NS::DonorField(saveSlots, L"button_back");
    if (!backDonor) {
        // Fail CLOSED and retry: a missing donor means we do not know what else moved in
        // this build. The browser owns the loud player-facing alarm for that condition;
        // this screen is reached THROUGH it, so a second dialog would only stack.
        if (++g_buildAttempts == 15)
            UE_LOGE("host_window_native: ui_saveSlots_C.button_back absent after %d "
                    "attempts -- NOT building", g_buildAttempts);
        return false;
    }

    void* root = NS::Spawn(P::name::UserWidgetClass, switcher);
    void* tree = root ? NS::Spawn(P::name::WidgetTreeClass, root) : nullptr;
    void* ovl  = tree ? NS::Spawn(L"Overlay", tree) : nullptr;
    if (!root || !tree || !ovl) return false;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(root) + P::off::UUserWidget_WidgetTree) = tree;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(tree) + P::off::UWidgetTree_RootWidget) = ovl;

    void* scrim = NS::Spawn(L"Image", ovl);
    if (!scrim) return false;
    g_scrimW = scrim;
    U::SetImageTintRaw(scrim, FLinearColor{0.f, 0.f, 0.f, 0.5f});
    E::SetWidgetVisibility(scrim, 0);
    if (void* s = U::AddChild(ovl, scrim))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                        NS::kFill, NS::kFill);

    void* winBox = NS::Spawn(L"SizeBox", ovl);
    void* winOvl = winBox ? NS::AddFramedBox(winBox, kPanel, kBorderPx) : nullptr;
    void* col    = winOvl ? NS::Spawn(L"VerticalBox", winOvl) : nullptr;
    if (!winBox || !winOvl || !col) return false;
    U::SetSizeBoxWidth(winBox, kWindowW);
    U::SetSizeBoxHeight(winBox, kWindowH);
    if (void* s = U::AddChild(winOvl, col)) {
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                        NS::kFill, NS::kFill);
        auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                             P::off::UOverlaySlot_Padding);
        pad[0] = pad[1] = pad[2] = pad[3] = kPadPx;
    }
    U::SetContent(winBox, winOvl);
    if (void* s = U::AddChild(ovl, winBox))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                        NS::kCenter, NS::kCenter);

    // Title strip + X, the browser's shape so the two screens match.
    if (void* titleBox = NS::AddFramedBox(col, kPanel, kBorderPx)) {
        if (void* titleRow = NS::Spawn(L"HorizontalBox", titleBox)) {
            NS::AddText(titleRow, L"Multivoid  -  Host Game", 24, kText, NS::kJustCenter, 1.f);
            g_closeBtn = NS::BuildButton(titleRow, backDonor, L"X", 20);
            if (void* s = U::AddChild(titleBox, titleRow))
                U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                                NS::kFill, NS::kCenter);
        }
        if (void* s = U::AddChild(col, titleBox)) {
            auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                                 P::off::UVerticalBoxSlot_Padding);
            pad[0] = pad[1] = pad[2] = 0.f; pad[3] = kPadPx;
        }
    }

    NS::AddText(col, L"WORLD", 16, kAccent, NS::kJustLeft, 0.f);
    g_newGameRow = BuildRow(col, 1.f, 0.f, 0.f);
    SetText(g_newGameRow.a, L"New game", kText);

    void* listBox = NS::Spawn(L"SizeBox", col);
    g_list = listBox ? NS::Spawn(L"ScrollBox", listBox) : nullptr;
    if (!listBox || !g_list) return false;
    U::SetSizeBoxHeight(listBox, kListH);
    U::SetContent(listBox, g_list);
    U::AddChild(col, listBox);

    NS::AddText(col, L"CONNECTION", 16, kAccent, NS::kJustLeft, 0.f);
    for (int i = 0; i < 3; ++i) {
        Row r = BuildRow(col, 0.42f, 0.58f, 0.f);
        g_connRow[i]   = r.bg;
        g_connLabel[i] = r.a;
        SetText(r.a, kConnModes[i].title,  kText);
        SetText(r.b, kConnModes[i].detail, kDim);
    }

    // Footer: BACK at the LEFT (style doc section 5 -- bottom-right is the CONFIRM
    // position in every native window), HOST at the right, status between them.
    if (void* footBox = NS::AddFramedBox(col, kPanel, kBorderPx)) {
        if (void* footRow = NS::Spawn(L"HorizontalBox", footBox)) {
            g_backBtn = NS::BuildButton(footRow, backDonor, L"BACK", 18);
            g_status  = NS::AddText(footRow, L"", 16, kText, NS::kJustLeft, 1.f);
            g_hostBtn = NS::BuildButton(footRow, backDonor, L"HOST", 18);
            if (void* s = U::AddChild(footBox, footRow))
                U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                                NS::kFill, NS::kCenter);
        }
        U::AddChild(col, footBox);
    }

    g_root = root;
    UE_LOGI("host_window_native: screen built (root=%p list=%p) after %d attempt(s)",
            root, g_list, g_buildAttempts + 1);
    return true;
}

// ============================ data ===================================================

void SyncSaves() {
    const uint64_t rev = sb::CopySaves(g_saves);
    if (rev == g_savesRev && !g_saveRows.empty()) return;
    g_savesRev = rev;

    const int want = static_cast<int>(g_saves.size()) > kMaxSaveRows
                         ? kMaxSaveRows : static_cast<int>(g_saves.size());
    // What the hover walk is allowed to consider. `g_saveRows.size()` is a HIGH-WATER MARK
    // -- rows are grown and never removed, only Collapsed -- and a collapsed widget keeps
    // the rect it last painted with, so passing the vector size let a click in the empty
    // area under the live rows hover a row that is not on screen.
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
    // NOTHING TO REPAIR HERE ANY MORE. The selection is a slot NAME, so a re-sort, a
    // shrink and a rescan all leave it pointing at the same world -- or at no world, if
    // that save is gone, which `SlotIndex` reports by returning -1.

    RepaintAll();
}

// ============================ input ==================================================

void UpdateHover() {
    // THE TRACKER, not a cursor-delta of our own. This screen had
    //     if (p == g_lastCursor) return;
    // over a SCROLLABLE list -- 24 rows of 56 px in a 300 px viewport -- so a wheel turn
    // slid a different world under a stationary pointer while the stored index stayed put,
    // and the stored index is what HOST loads. The browser had the same gate, was fixed,
    // and the fix was not carried across; the tracker exists so it cannot happen a third
    // time. It also supplies the settling pass the Slate-hover checks below have always
    // needed: Slate's hover reads one tick behind the pointer, so evaluating only on a
    // moving tick left New Game and the three mode rows lighting up a frame late and never
    // correcting once the cursor stopped.
    if (!g_hover.Poll(g_list, g_visibleSaves)) return;

    const int prevRow = g_hoverRow, prevConn = g_hoverConn;
    g_hoverRow  = -2;
    g_hoverConn = -1;
    // NEW GAME and the connection rows sit in `col`, OUTSIDE the ScrollBox, so Slate's own
    // hover answers for them and they keep asking it. The SAVE rows are inside `g_list` and
    // it does NOT: a row background there is a UImage set Visible whose rect contains the
    // cursor, and `IsHovered()` on it reads 0 (measured 2026-08-29 on the server browser,
    // which shipped the identical construct the same day). Left as it was, the world list
    // could not be clicked at all -- this window could only ever start a NEW game.
    if (g_newGameRow.bg && E::WidgetIsHovered(g_newGameRow.bg)) g_hoverRow = -1;
    if (g_hoverRow == -2 && g_hover.Index() >= 0) g_hoverRow = g_hover.Index();
    for (int i = 0; i < 3; ++i)
        if (g_connRow[i] && E::WidgetIsHovered(g_connRow[i])) { g_hoverConn = i; break; }
    if (g_hoverRow != prevRow || g_hoverConn != prevConn) RepaintAll();
}

void DoHost() {
    sm::SaveChoice c;
    const int sel = SlotIndex();
    if (sel < 0) {
        c.newGame = true;
        c.newName = "Coop";
        c.mode    = 0;   // enum_gamemode story
    } else {
        c.newGame = false;
        c.slot    = Narrow(g_saves[static_cast<size_t>(sel)].slot);
    }
    // THE NAME. v1 has no text entry (see the header), so it is derived rather than typed:
    // the browser lists servers by name and "<nick>'s game" tells another player who is
    // hosting, which a fixed literal cannot. Renaming lives on the ImGui surface until a
    // focusable native field is measured.
    const std::string name = sm::Nickname().empty() ? "Multivoid game"
                                                    : sm::Nickname() + "'s game";
    // ONE ACTION, TWO VIEWS: this is the same call the ImGui picker makes. No hosting
    // rule is authored here -- if one must change, it changes in session_manager.
    const bool accepted = sm::HostWithSave(c, name, /*locked=*/false, /*playersMax=*/4,
                                           /*directConnection=*/g_connMode == 1,
                                           /*hideFromBrowser=*/false,
                                           /*lanOnly=*/g_connMode == 2);
    UE_LOGI("host_window_native: HOST %s -- world=%s conn=%d name='%s'",
            accepted ? "accepted" : "REFUSED (another action in flight)",
            c.newGame ? "<new game>" : c.slot.c_str(), g_connMode, name.c_str());
    if (!accepted) {
        // The window STAYS OPEN on a refusal. A screen that closes on failure is how the
        // user's "nothing told about the session being DEAD" happened: the only surface
        // showing the reason was the one that had just gone away.
        SetStatus(L"Busy -- another host or join is already starting.");
        return;
    }
    SetStatus(L"Starting...");
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

    if (g_closeBtn && E::WidgetIsHovered(g_closeBtn)) { Hide("X"); return; }
    if (g_backBtn  && E::WidgetIsHovered(g_backBtn))  { Hide("BACK"); return; }
    if (g_hostBtn  && E::WidgetIsHovered(g_hostBtn))  { DoHost(); return; }
    for (int i = 0; i < 3; ++i)
        if (g_connRow[i] && E::WidgetIsHovered(g_connRow[i])) { g_connMode = i; RepaintAll(); return; }
    if (g_newGameRow.bg && E::WidgetIsHovered(g_newGameRow.bg)) {
        g_selectedSlot.clear(); RepaintAll(); return;
    }
    // The row under the cursor is already known from the hover pass, which asked geometry
    // rather than Slate for exactly the reason recorded there. Re-deriving it here would be
    // a second implementation of the same question, and the two could disagree.
    // BOUNDED BY THE SAVES, not by the row widgets: the row vector is a high-water mark and
    // `g_saves` is the truth. A hover index outside it is a collapsed row and selects nothing
    // -- which also stops this branch being the silent fall-through for every click that hits
    // no control at all.
    if (g_hoverRow >= 0 && g_hoverRow < static_cast<int>(g_saves.size())) {
        g_selectedSlot = g_saves[static_cast<size_t>(g_hoverRow)].slot;
        RepaintAll();
        return;
    }
}

// ============================ lifecycle ==============================================

void Show() {
    if (!g_switcher || !g_root || g_shown) return;
    g_priorIndex = U::SwitcherIndex(g_switcher);
    if (g_ourIndex < 0) {
        // ASK THE PANEL WHERE OUR WIDGET IS. Do not infer it from the child count.
        //
        // This was `AddChild(...); g_ourIndex = ChildCount(...) - 1;` -- correct only while
        // two things hold that nothing checks: that the add SUCCEEDED, and that nothing is
        // appended after us. `AddChild`'s return value was discarded, so a failed add left
        // `g_ourIndex` naming the LAST OF THE GAME'S OWN SCREENS, cached for the menu's
        // life. MEASURED 2026-08-30, hands-on: clicking MULTIPLAYER opened VOTV's Stats
        // panel, and the log said `shown (index 0 -> 10)` where a healthy run says 11.
        //
        // Two screens of ours now add to this one switcher, so "last child" is an assumption
        // about a container we no longer own alone. `GetChildIndex` answers the question
        // that was actually being asked, and answers it about OUR widget.
        void* slot = U::AddChild(g_switcher, g_root);
        g_ourIndex = U::IndexOfChild(g_switcher, g_root);
        if (g_ourIndex < 0) {
            // FAIL CLOSED. Activating an index we cannot prove is ours means showing one of
            // the game's screens from our button -- which is worse than doing nothing,
            // because it looks like the menu is broken rather than like we are.
            UE_LOGE("%s: could not place %s in the menu switcher (AddChild slot=%p, "
                    "GetChildIndex=-1). NOT switching: activating an unproven index would "
                    "open one of the game's own screens.", "host_window_native", "the hosting window", slot);
            ue_wrap::log::Flush();
            return;
        }
    }
    U::SwitcherSetIndex(g_switcher, g_ourIndex);
    g_shown = true;
    // FORGET THE OLD HOVER. Reopening does not move the pointer, so without this the index
    // from the last time this window was up survives -- and the click path reads it, so a
    // click on inert chrome would select whatever row the player happened to leave the
    // cursor over minutes ago.
    g_hover.Reset();
    g_hoverRow  = -2;
    g_hoverConn = -1;
    g_escPrimed = false;
    g_lmbPrimed = false;
    sb::RefreshAsync();          // the list is stale by definition between openings
    SyncSaves();
    // EDGE-GATED. This rewrote an FText every tick -- two dispatches plus a wstring and an
    // FText per frame for a string that changes when a host attempt finishes.
    {
        static std::string sLast;
        std::string cur = sm::HostStatus();
        if (cur != sLast) { sLast = cur; SetStatus(Widen(cur)); }
    }
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

// An autonomous run cannot press a button, and this screen's real entry point (the
// browser's HOST control) does not exist yet -- so the lab reaches it the same way it
// reaches the browser.
bool AutoOpenArmed() {
    static const bool s =
        coop::config::ResolveFlag(::coop::config_registry::rows::host_window_autoopen);
    return s && Armed();
}

}  // namespace

void Open()   { g_wantOpenMs.store(::GetTickCount64(), std::memory_order_relaxed); }
void Close()  { g_wantOpenMs.store(0, std::memory_order_relaxed);
                g_wantClose.store(true, std::memory_order_relaxed); }
bool IsOpen() { return g_shown; }

int   SelectedSave()   { return SlotIndex(); }
int   SaveRowCount()   { return g_visibleSaves; }   // shown, not the row vector's high-water mark
void* SaveListWidget() { return g_list; }

void OnMenuTick(void* menu, void* switcher) {
    if (!Armed() || !menu || !switcher) return;
    g_switcher = switcher;

    if (menu != g_menu) {
        g_menu = menu;
        g_root = nullptr; g_list = nullptr; g_status = nullptr; g_scrimW = nullptr;
        g_closeBtn = nullptr; g_backBtn = nullptr; g_hostBtn = nullptr;
        g_newGameRow = Row{};
        g_saveRows.clear();
        for (int i = 0; i < 3; ++i) { g_connRow[i] = nullptr; g_connLabel[i] = nullptr; }
        g_ourIndex = -1; g_shown = false; g_buildAttempts = 0; g_savesRev = 0;
    }
    if (!g_root) {
        if (!BuildScreen(switcher)) {
            // SAY SO. Since 2026-08-29 the browser's HOST button closes the browser
            // SYNCHRONOUSLY and then asks for this window, so a build that keeps failing
            // leaves the player on the main menu with no window, no browser and -- until
            // this line -- nothing in the log either. `Open()` returns void, so the button
            // cannot answer; the log is the only place the truth can go.
            // LATCHED. Unlatched, this ran on every menu tick for the intent's whole 20 s
            // TTL -- ~1200 lines, each one a synchronous fflush (log.cpp flushes every
            // non-INFO line). A diagnostic added to break a silence must not become the
            // next hot-path defect; the browser's equivalent latches on g_toldTheUser.
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
            // THE SAME HAND-OVER THE REAL DOOR PERFORMS. With both dev flags armed, the
            // browser also auto-opens, and opening on top of it makes this window record the
            // BROWSER's index as the one to restore -- the exact corruption `CloseNow` was
            // added to prevent, still reachable through the lab door. A test door that
            // behaves differently from the player's door is how a lab result lies.
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
            // The browser's expiry path logs; this one did not, so a HOST click that
            // arrived while no menu tick was coming vanished without trace.
            UE_LOGW("host_window_native: a HOST request expired unconsumed after %llu ms "
                    "(ttl %llu) -- no main-menu tick arrived to show the window",
                    static_cast<unsigned long long>(age),
                    static_cast<unsigned long long>(kIntentTtlMs));
    }
    if (!g_shown) return;

    // Reconcile against the LIVE index: a sibling screen navigating away is observed
    // rather than assumed, exactly as the browser does.
    if (U::SwitcherIndex(g_switcher) != g_ourIndex) { g_shown = false; return; }

    SyncSaves();
    UpdateHover();
    PollChrome();
    // The host status is authored on a worker thread and OUTLIVES this window's closing;
    // showing it here every tick is what makes a refusal visible where the action was
    // taken rather than in a window the player has already left.
    // EDGE-GATED. This rewrote an FText every tick -- two dispatches plus a wstring and an
    // FText per frame -- for a string that only changes when a host attempt finishes.
    if (g_status) {
        static std::string sLastStatus;
        std::string cur = sm::HostStatus();
        if (cur != sLastStatus) { sLastStatus = cur; SetStatus(Widen(cur)); }
    }
}

}  // namespace ui::host_window_native
