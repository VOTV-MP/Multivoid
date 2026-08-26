// ui/server_browser_native.cpp -- see ui/server_browser_native.h.

#include "ui/server_browser_native.h"

#include "coop/config/config.h"
#include "coop/net/protocol.h"          // kProtocolVersion -- the version-cell mismatch tint
#include "coop/session/session_manager.h"
#include "ui/boot_warning_dialog.h"     // the loud failure surface for a donor that never appears
#include "ui/input_focus.h"            // a click only counts while OUR window is foreground
#include "ui/server_browser_selftest.h"  // the dev phase machine; ships dark
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <string>
#include <vector>

namespace ui::server_browser_native {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace U  = ue_wrap::umg;
namespace P  = ue_wrap::profile;
namespace sm = coop::session_manager;
namespace selftest = ui::server_browser_selftest;

using ue_wrap::FLinearColor;
using Row = coop::net::lobby::LobbyRow;

// ---- layout constants ------------------------------------------------------------
// Slate units. The row height is the game's own (uicomp_saveSlot_C is 64 px, section 7b),
// so a row of ours is the same size as a row of theirs.
constexpr float kWindowW  = 980.f;
constexpr float kWindowH  = 620.f;
constexpr float kRowH     = 64.f;
// Frame + spacing, from the native windows (style doc section 3).
constexpr float kBorderPx = 2.f;
constexpr float kPadPx    = 6.f;
constexpr float kRowGapPx = 2.f;
// The list's height is EXPLICIT, not the VerticalBox's leftover slack.
//
// MEASURED 2026-08-26: with a Fill slot the box allotted the ScrollBox 542 px inside a
// window that only had ~484 px left for it (offsetOfEnd 1438 against 30 rows of 66 px puts
// the viewport at 542), so the list overflowed UPWARD and its first row was drawn clipped
// under the column header. Slack arithmetic depends on every sibling's desired size being
// what you assumed; an override depends on nothing.
constexpr float kListH    = 470.f;
// BOUNDS THE WHOLE SYNC LOOP, not just the display: `want` clamps to this, the grow
// loop is bounded by `want`, and `total = ChildCount` therefore never exceeds it. So
// this is a COMPUTE ceiling that happens to look like a display one.
// The comment that stood here -- "the master caps its list well below this" -- was
// FALSE (2026-08-26): `build_rows` (master.rs:531-553) emits every listed lobby,
// unbounded, so 100 servers render 64 and nothing logs the truncation.
// Raising it is step T2b and it CANNOT come first: see docs/MULTIPLAYER_UI.md
// section 8c.-1, which measures why the uncached walk per row must go first --
// raising the cap before that converts a latent defect into a 110-320 ms stall.
constexpr int   kMaxRows  = 64;
// EHorizontalAlignment / EVerticalAlignment: Fill=0 Left=1/Top=1 Center=2 Right=3/Bottom=3.
constexpr uint8_t kFill = 0, kLeft = 1, kCenter = 2;
// ETextJustify: Left=0 Center=1 Right=2.
constexpr uint8_t kJustLeft = 0, kJustCenter = 1;

// ---- the palette ------------------------------------------------------------------
// docs/VOTV_UI_STYLE.md section 2. Every value is SAMPLED from the game's own menus by
// histogram, and the set is a designed ramp rather than an accumulation -- #1A1A1A /
// #313131 / #404040 step evenly, and #400040 (selected) and #400000 (destructive) are one
// 0x40 component moved between channels. A colour that is not in that table is a mistake.
//
// THE sRGB CONVERSION IS NOT OPTIONAL. Those are sRGB byte values; FLinearColor is LINEAR,
// and the framebuffer converts back on the way out. Writing 0x31/255 = 0.192 as a linear
// tint puts sRGB 0.48 on screen -- #7B, more than double the intended #31 -- so the whole
// palette would render washed out and no amount of re-picking values would fix it. This is
// the same transform UE's FLinearColor::FromSRGBColor applies.
inline FLinearColor Srgb(int r, int g, int b, float a = 1.f) {
    auto f = [](int v) {
        const float c = static_cast<float>(v) / 255.f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return FLinearColor{f(r), f(g), f(b), a};
}

const FLinearColor kPanel  = Srgb(0x1A, 0x1A, 0x1A);        // window fill
const FLinearColor kBorder = Srgb(0x64, 0x64, 0x64);        // every frame in the game's menus
const FLinearColor kRowBg  = Srgb(0x31, 0x31, 0x31);        // a list row at rest
const FLinearColor kRowSel = Srgb(0x40, 0x00, 0x40);        // ...and selected. Fill, not text.
const FLinearColor kText   = Srgb(0xFF, 0xFF, 0xFF);        // the default: most text is white
const FLinearColor kAccent = Srgb(0xFF, 0x7C, 0x00);        // orange -- the interactive accent
const FLinearColor kHover  = Srgb(0xFF, 0xFF, 0x00);        // hover is a TEXT colour, see section 4
const FLinearColor kAmber  = Srgb(0xFF, 0xBC, 0x00);        // value emphasis; the mismatch tint
const FLinearColor kDim    = Srgb(0xA5, 0xA5, 0xA5);        // secondary text (measured, not guessed)
const FLinearColor kOwn    = Srgb(0x9E, 0xEA, 0xB3);        // "your server" row

// ---- state (GAME THREAD ONLY unless marked) --------------------------------------
void* g_menu     = nullptr;   // the ui_menu_C we built into (compared, never dereferenced)
void* g_switcher = nullptr;
void* g_root     = nullptr;   // our UUserWidget
void* g_list     = nullptr;   // the UScrollBox holding the rows
void* g_status   = nullptr;   // the footer UTextBlock
void* g_title    = nullptr;
void* g_scrimW   = nullptr;   // the full-screen scrim -- the thing that absorbs a stray click
void* g_closeBtn = nullptr;   // the X, top-right of the title row
void* g_backBtn  = nullptr;   // BACK, bottom-right beside the status line
// LBUTTON edge state for the chrome poll. Primed on Show() so the very release that
// OPENED the screen cannot be read as a click on the X sitting under the cursor.
bool  g_prevLmb   = false;
bool  g_lmbPrimed = false;

// ---- row state (style doc section 4: hover and selection are DIFFERENT channels) ----
// Hover recolours the row's TEXT; selection recolours the row's FILL. Both are applied on
// a CHANGE, never per frame.
int         g_hoverRow   = -1;   // index into the live rows, or -1
std::string g_selectedId;        // the SELECTED LOBBY, keyed by id and not by index
POINT       g_lastCursor{-1, -1};
bool        g_hoverPending = false;   // one settling pass is owed after motion stops
// ESC edge state. Primed on the first tick a screen is shown so a key already held when it
// opens cannot synthesize a close -- the same guard multiplayer_menu's click poll uses.
bool  g_prevEsc   = false;
bool  g_escPrimed = false;
int32_t g_ourIndex   = -1;
int32_t g_priorIndex = -1;
bool    g_shown      = false;

// Row identity. See the header, invariant 1: `g_rowIds[i]` is the lobbyId the child at
// index i was RENDERED with, written in the same pass as that child's text by the single
// function that owns the panel. Positional INSIDE this one structure is fine; positional
// across the network list and the displayed list is the defect this exists to avoid.
std::vector<std::string> g_rowIds;
std::vector<Row> g_rows;
uint64_t g_lastRefreshMs = 0;
int      g_buildAttempts = 0;
bool     g_toldTheUser   = false;

// Cross-thread: the deferred open intent and the pointer-moved flag.
std::atomic<uint64_t> g_wantOpenMs{0};   // 0 = no intent
std::atomic<bool>     g_wantClose{false};
constexpr uint64_t kIntentTtlMs  = 20000;  // a join that never returns to a menu must expire
constexpr uint64_t kRefreshMs    = 1000;

inline void* ReadPtr(void* base, int32_t off) {
    return (base && off >= 0) ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off)
                              : nullptr;
}

// A sub-screen donor MUST be read off the switcher's own child list. `R::FindObjectByClass`
// returns a DIFFERENT non-CDO instance -- a WidgetBlueprint carries a widget-tree template
// that is not named `Default__`, so the CDO skip at reflection.cpp:511 does not exclude it
// -- and every donor field read through it comes back null. That produced a false finding
// on the probe's first run and would have forced this design to add a precondition it
// explicitly rejected. Measured 2026-08-25.
void* SwitcherChild(void* switcher, const wchar_t* className) {
    const int32_t n = U::ChildCount(switcher);
    for (int32_t i = 0; i < n && i < 64; ++i) {
        void* c = U::ChildAt(switcher, i);
        if (c && R::ClassNameOf(c) == className) return c;
    }
    return nullptr;
}

void* DonorField(void* owner, const wchar_t* field) {
    if (!owner) return nullptr;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(owner), field);
    return ReadPtr(owner, off);
}

void* Spawn(const wchar_t* cls, void* outer) {
    void* k = R::FindClass(cls);
    return k ? E::SpawnUObject(k, outer) : nullptr;
}

// Build one styled UTextBlock and put it in `panel`, with an optional horizontal-box slot
// fill weight (0 = leave the slot alone, i.e. auto-size).
void* AddText(void* panel, const wchar_t* initial, int32_t size, const FLinearColor& col,
              uint8_t justify, float fillWeight) {
    void* t = Spawn(P::name::TextBlockClass, panel);
    if (!t) return nullptr;
    U::StyleTextBlock(t, size, col, justify);
    E::SetWidgetText(t, initial);
    void* slot = U::AddChild(panel, t);
    if (slot && fillWeight > 0.f) {
        auto* s = reinterpret_cast<uint8_t*>(slot) + P::off::UHorizontalBoxSlot_Size;
        *reinterpret_cast<float*>(s + P::off::FSlateChildSize_Value) = fillWeight;
        *(s + P::off::FSlateChildSize_SizeRule) = 1;  // ESlateSizeRule::Fill
        U::SetSlotAlign(slot, P::off::UHorizontalBoxSlot_HAlign,
                        P::off::UHorizontalBoxSlot_VAlign, kFill, kCenter);
        // The SLOT bounds the layout, not the painting: without this a long world name
        // paints straight across the Age column. EWidgetClipping::ClipToBounds = 1.
        U::SetClipping(t, 1);
        // ...and clipping alone leaves a clipped value touching the next column, which
        // reads as a rendering fault rather than as a long name. FMargin is
        // {Left, Top, Right, Bottom}; a right gutter is the whole fix.
        auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(slot) +
                                             P::off::UHorizontalBoxSlot_Padding);
        pad[0] = 0.f; pad[1] = 0.f; pad[2] = 18.f; pad[3] = 0.f;
    }
    return t;
}

// THE FRAME. Every panel, row, header strip and value cell in VOTV's menus is a bordered
// box with sharp corners (style doc section 3), and nothing in the game's UI floats
// unboxed. Two stacked UImages give exactly that: an outer one carrying the border colour
// and an inner one inset by the border width carrying the fill. A UImage with no
// ResourceObject and only a tint draws a solid rect, which is how the game's own
// full-screen scrim works -- so this needs no donor and no art.
//
// Returns the OVERLAY the caller should put content in; content lands above the fill.
void* AddFramedBox(void* parent, const FLinearColor& fill, float borderPx) {
    void* box = Spawn(L"Overlay", parent);
    if (!box) return nullptr;
    void* edge = Spawn(L"Image", box);
    void* face = Spawn(L"Image", box);
    if (!edge || !face) return nullptr;
    U::SetImageTintRaw(edge, kBorder);
    U::SetImageTintRaw(face, fill);
    // ESlateVisibility: Visible=0 Collapsed=1 HIDDEN=2 HitTestInvisible=3
    // SelfHitTestInvisible=4. The first version of this wrote 2 meaning "chrome, not a hit
    // target" and got HIDDEN -- so the frame and the panel fill never drew AT ALL, and the
    // screenshot's apparent "window" was nothing but the rows' own backgrounds stacked up
    // with the title and footer floating outside them. 3 is the one that means what 2 was
    // meant to mean: it draws, and it never eats a click.
    E::SetWidgetVisibility(edge, 3);
    E::SetWidgetVisibility(face, 3);
    if (void* s = U::AddChild(box, edge))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
    if (void* s = U::AddChild(box, face)) {
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
        auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                             P::off::UOverlaySlot_Padding);
        pad[0] = pad[1] = pad[2] = pad[3] = borderPx;
    }
    return box;
}

// A chrome UButton with a text label, styled from a donor UButton.
//
// A REAL UButton, not a text block with a click poll, and the reason is the SOUND. Cloning
// the style carries the game's own press and hover FSlateSounds, so our X clicks like the
// menu's own controls -- the same thing that makes the shipped MULTIPLAYER inject feel
// native. It also gets Slate's press visual for free.
//
// The LABEL is authored, never cloned. Cloning a donor UTextBlock's style was tried on the
// menu inject and REVERTED: the donor reads null at some timings and the silent fallback is
// Roboto/centred/white. StyleTextBlock writes the measured constants instead.
//
// The label's PADDING is what gives the button its hit area -- "X" is one glyph, and an
// unpadded button is a hit target the width of that glyph.
void* BuildButton(void* parent, void* donorBtn, const wchar_t* label, int32_t fontSize) {
    void* b = Spawn(P::name::ButtonClass, parent);
    if (!b) return nullptr;
    U::CloneButtonStyle(b, donorBtn);
    if (void* t = Spawn(P::name::TextBlockClass, b)) {
        U::StyleTextBlock(t, fontSize, kText, kJustCenter);
        E::SetWidgetText(t, label);
        if (void* cw = R::FindClass(P::name::ContentWidgetClass)) {
            if (void* fn = R::FindFunction(cw, P::name::SetContentFn)) {
                ue_wrap::ParamFrame f(fn);
                f.Set<void*>(L"Content", t);
                Call(b, f);
            }
        }
        // SetContent created the UButtonSlot; centre the glyph and pad it out.
        if (void* cslot = ReadPtr(t, static_cast<int32_t>(P::off::UWidget_Slot))) {
            auto* cs = reinterpret_cast<uint8_t*>(cslot);
            *(cs + P::off::UButtonSlot_HAlign) = kCenter;
            *(cs + P::off::UButtonSlot_VAlign) = kCenter;
            auto* pad = reinterpret_cast<float*>(cs + P::off::UButtonSlot_Padding);
            pad[0] = 16.f; pad[1] = 6.f; pad[2] = 16.f; pad[3] = 6.f;
        }
    }
    if (void* s = U::AddChild(parent, b))
        U::SetSlotAlign(s, P::off::UHorizontalBoxSlot_HAlign,
                        P::off::UHorizontalBoxSlot_VAlign, kCenter, kCenter);
    return b;
}

// The five columns, in the order section 7c fixes them, with their fill weights.
struct Column { const wchar_t* title; float weight; };
constexpr Column kColumns[5] = {
    {L"Name",    0.44f},
    {L"Players", 0.12f},
    {L"Version", 0.20f},
    {L"World",   0.14f},
    {L"Age",     0.10f},
};

// ---- the screen -------------------------------------------------------------------

// Build one row: USizeBox(64) -> UOverlay -> [ UImage background (the HIT TARGET),
// UHorizontalBox of five UTextBlocks ]. NO UButton: the native row's own `button_select`
// draws nothing in all three states, `IsHovered()` is owned by UWidget so a bare UImage
// answers it (measured), and a UButton would add a press visual we would then suppress.
void* BuildRow(void* parent) {
    void* box = Spawn(L"SizeBox", parent);
    if (!box) return nullptr;
    U::SetSizeBoxHeight(box, kRowH);
    void* ovl = Spawn(L"Overlay", box);
    void* bg  = ovl ? Spawn(L"Image", ovl) : nullptr;
    void* hb  = ovl ? Spawn(L"HorizontalBox", ovl) : nullptr;
    if (!ovl || !bg || !hb) return nullptr;
    // The background carries the row's state tint AND is the hit target, so it must be
    // Visible -- a SelfHitTestInvisible image answers IsHovered() == false, which would
    // look exactly like the whole approach failing.
    U::SetImageTintRaw(bg, kRowBg);
    E::SetWidgetVisibility(bg, 0);  // ESlateVisibility::Visible
    if (void* s = U::AddChild(ovl, bg))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
    if (void* s = U::AddChild(ovl, hb))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
    for (const Column& c : kColumns)
        AddText(hb, L"", 16, kText, kJustLeft, c.weight);
    // SizeBox is a UContentWidget: its single child goes through SetContent.
    if (void* cw = R::FindClass(P::name::ContentWidgetClass)) {
        if (void* fn = R::FindFunction(cw, P::name::SetContentFn)) {
            ue_wrap::ParamFrame f(fn);
            f.Set<void*>(L"Content", ovl);
            Call(box, f);
        }
    }
    return box;
}

// A row's parts, re-derived from the panel on demand. We hold NO row pointers across ticks
// (cached_obj_ref.h:34-46 -- the world stamp is inert for UMG, and UE assigns serials
// lazily, so a hand-spawned widget captures serial 0 and the ABA residual is not closed).
// The panel's `Slots` is the authority; this walks it.
struct RowParts { void* box; void* bg; void* text[5]; };
bool RowPartsAt(int32_t i, RowParts& out) {
    out = RowParts{};
    void* box = U::ChildAt(g_list, i);
    if (!box) return false;
    out.box = box;
    void* ovl = nullptr;
    if (void* cw = R::FindClass(P::name::ContentWidgetClass)) {
        if (void* fn = R::FindFunction(cw, L"GetContent")) {
            ue_wrap::ParamFrame f(fn);
            if (Call(box, f)) ovl = f.Get<void*>(L"ReturnValue");
        }
    }
    if (!ovl) return false;
    out.bg = U::ChildAt(ovl, 0);
    void* hb = U::ChildAt(ovl, 1);
    if (!hb) return false;
    for (int c = 0; c < 5; ++c) out.text[c] = U::ChildAt(hb, c);
    return true;
}

// The version cell, PORTED from the incumbent rather than re-derived (server_browser.cpp:
// 218-231). `game` is the host's VOTV target and `version` is a LEGACY-ONLY fallback that
// only a pre-field host still sends; the mismatch has TWO legs and amber always means
// "the join gate will refuse this".
std::wstring VersionCell(const Row& r, bool& mismatch) {
    const bool gameMismatch = !r.game.empty() && r.game != sm::GameTarget();
    mismatch = gameMismatch ||
               (r.proto > 0 && r.proto != static_cast<int>(coop::net::kProtocolVersion));
    std::string cell = !r.game.empty() ? r.game : r.version;
    if (r.proto > 0) cell += " b" + std::to_string(r.proto);
    if (mismatch) cell += " (!)";
    return std::wstring(cell.begin(), cell.end());
}

// A row's five text colours, from its data and its hover state. ONE owner: the sync pass
// and the hover edge both come through here, so "what colour is the version cell" is
// answered in exactly one place and un-hovering cannot restore the wrong base.
void ApplyRowTextColors(const RowParts& rp, const Row& r, bool isOwn, bool mismatch,
                        bool hovered) {
    // Style doc section 4: hovering turns the LABEL yellow and leaves the fill alone.
    // That is the opposite of the ImGui incumbent, which paints behind the row.
    const FLinearColor name = hovered ? kHover : (isOwn ? kOwn : kText);
    const FLinearColor body = hovered ? kHover : kText;
    const FLinearColor dim  = hovered ? kHover : kDim;
    const FLinearColor ver  = hovered ? kHover : (mismatch ? kAmber : kDim);
    if (rp.text[0]) E::SetTextBlockColor(rp.text[0], name);
    if (rp.text[1]) E::SetTextBlockColor(rp.text[1], body);
    if (rp.text[2]) E::SetTextBlockColor(rp.text[2], ver);
    if (rp.text[3]) E::SetTextBlockColor(rp.text[3], dim);
    if (rp.text[4]) E::SetTextBlockColor(rp.text[4], dim);
    (void)r;
}

void SetRowText(void* block, const std::string& utf8, const FLinearColor& col) {
    if (!block) return;
    const std::wstring w(utf8.begin(), utf8.end());
    E::SetWidgetText(block, w.c_str());
    E::SetTextBlockColor(block, col);
}

// Repaint every row's FILL from the current selection. Cheap (one tint per row, no
// reflection walk) and only ever called on a selection change.
void RepaintRowFills() {
    const int total = U::ChildCount(g_list);
    for (int i = 0; i < total; ++i) {
        RowParts rp;
        if (!RowPartsAt(i, rp) || !rp.bg) continue;
        const bool sel = !g_selectedId.empty() &&
                         i < static_cast<int>(g_rowIds.size()) &&
                         g_rowIds[static_cast<size_t>(i)] == g_selectedId;
        U::SetImageTint(rp.bg, sel ? kRowSel : kRowBg);
    }
}

// WHICH ROW IS UNDER THE CURSOR, evaluated ONLY when the cursor has actually moved.
//
// There is no cheaper way to ask. `FGeometry` has no reflected members (size 0x38, empty),
// so the rows' screen rects are not readable and hit-testing has to go through Slate's own
// IsHovered(). That is a UFunction dispatch per row, which is precisely the per-row cost
// the open perf lane is about -- so it is gated twice: once on the cursor having moved at
// all, and once on the LIST being hovered, which is a single dispatch that answers "no"
// for every frame the pointer is anywhere else on the screen. The walk also stops at the
// first hit. A stationary cursor costs zero dispatches.
//
// `NotePointerMoved()` used to exist for this and was never wired to anything -- no
// caller, no consumer. GetCursorPos here is one syscall, needs no edit to the overlay's
// input path, and sees movement regardless of how the message was routed; the dead seam is
// retired with it (RULE 2).
void UpdateHover() {
    POINT c{};
    if (!::GetCursorPos(&c)) return;
    const bool moved = (c.x != g_lastCursor.x || c.y != g_lastCursor.y);
    g_lastCursor = c;

    // INVARIANT 2 (this module's header, measured): Slate's hover state is one tick behind
    // the pointer, so a reading taken in the same tick as the move describes where the
    // cursor WAS. Evaluating only on the moving tick would therefore leave the highlight
    // permanently one move behind -- and, worse, STUCK there: the next tick sees no delta,
    // skips the pass, and nothing ever corrects it.
    //
    // So evaluate on every moving tick AND on one settling tick after motion stops. During
    // a sweep the highlight trails by a frame, which is 8.5 ms and invisible; when the
    // cursor stops, the settling pass lands it on the final row.
    if (!moved && !g_hoverPending) return;   // stationary and already settled: free
    g_hoverPending = moved;

    int hit = -1;
    if (g_list && E::WidgetIsHovered(g_list)) {
        const int total = U::ChildCount(g_list);
        // THE PREVIOUS ROW IS PROBED FIRST, and that is what makes this affordable. Slate
        // gives us no readable geometry (FGeometry has no reflected members), so the only
        // hit-test is a UFunction per row -- the exact per-row cost the open perf lane is
        // about. During a sweep the cursor is still on the SAME row on most frames, so one
        // dispatch answers; the full walk with its early exit is the fallback.
        if (g_hoverRow >= 0 && g_hoverRow < total) {
            RowParts rp;
            if (RowPartsAt(g_hoverRow, rp) && rp.bg && E::WidgetIsHovered(rp.bg))
                hit = g_hoverRow;
        }
        for (int i = 0; hit < 0 && i < total; ++i) {
            if (i == g_hoverRow) continue;   // already probed
            RowParts rp;
            if (!RowPartsAt(i, rp) || !rp.bg) continue;
            if (E::WidgetIsHovered(rp.bg)) { hit = i; break; }
        }
    }
    if (hit == g_hoverRow) return;

    const int prev = g_hoverRow;
    g_hoverRow = hit;
    // Edge-applied: only the two rows that changed are recoloured.
    for (int i : {prev, hit}) {
        if (i < 0 || i >= static_cast<int>(g_rows.size())) continue;
        RowParts rp;
        if (!RowPartsAt(i, rp)) continue;
        const Row& r = g_rows[static_cast<size_t>(i)];
        const std::string own = sm::OwnLobbyId();
        bool mismatch = false;
        (void)VersionCell(r, mismatch);
        ApplyRowTextColors(rp, r, !own.empty() && r.lobbyId == own, mismatch, i == hit);
    }
}

// THE SINGLE WRITER of both the children's text and g_rowIds. Nothing else touches either,
// which is what makes the positional pairing safe (header, invariant 1).
void SyncRows() {
    sm::CopyRows(g_rows);
    const int want = static_cast<int>(g_rows.size()) > kMaxRows ? kMaxRows
                                                               : static_cast<int>(g_rows.size());
    int have = U::ChildCount(g_list);
    if (have < 0) have = 0;
    // GROW by building; SHRINK by COLLAPSING, never by detaching. Nothing roots a detached
    // row -- the panel's Slots is the only reference -- so a detached free-list would be a
    // use-after-free waiting for the next GC. A collapsed row stays rooted, is not
    // hit-testable, and a regrow just un-collapses it.
    for (int i = have; i < want; ++i) {
        void* row = BuildRow(g_list);
        if (!row) break;
        if (void* s = U::AddChild(g_list, row)) {
            U::SetSlotAlign(s, P::off::UScrollBoxSlot_HAlign, P::off::UScrollBoxSlot_VAlign,
                            kFill, kCenter);
            // ROW SEPARATION WITHOUT A PER-ROW BORDER, and this is a stated trade. Native
            // rows each carry their own frame; reproducing that costs one more UImage per
            // row, and this list's per-row cost is the exact subject of the open perf lane
            // (MULTIPLAYER_UI section 8c.-1). A gap that lets the darker panel show through
            // reads as a boxed row at zero extra widgets. If T6 lands a viewport pool, the
            // widget budget stops mattering and the real frame can replace this.
            auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                                 P::off::UScrollBoxSlot_Padding);
            pad[0] = 0.f; pad[1] = 0.f; pad[2] = 0.f; pad[3] = kRowGapPx;
        }
    }
    const int total = U::ChildCount(g_list);
    g_rowIds.assign(total < 0 ? 0 : total, std::string());

    const std::string own = sm::OwnLobbyId();
    for (int i = 0; i < total; ++i) {
        RowParts rp;
        if (!RowPartsAt(i, rp)) continue;
        if (i >= want) {
            E::SetWidgetVisibility(rp.box, 1);  // ESlateVisibility::Collapsed
            continue;
        }
        E::SetWidgetVisibility(rp.box, 0);
        const Row& r = g_rows[static_cast<size_t>(i)];
        const bool isOwn = !own.empty() && r.lobbyId == own;
        bool mismatch = false;
        const std::wstring ver = VersionCell(r, mismatch);

        SetRowText(rp.text[0], isOwn ? r.name + "   (your server)" : r.name, kText);
        SetRowText(rp.text[1], std::to_string(r.playersCur) + "/" + std::to_string(r.playersMax),
                   kText);
        if (rp.text[2]) E::SetWidgetText(rp.text[2], ver.c_str());
        SetRowText(rp.text[3], r.world, kDim);
        SetRowText(rp.text[4], std::to_string(r.ageSec) + "s", kDim);
        ApplyRowTextColors(rp, r, isOwn, mismatch, i == g_hoverRow);
        // SELECTION IS KEYED ON THE LOBBY, NOT THE ROW INDEX. The master returns lobbies
        // from a HashMap and nothing sorts them (MULTIPLAYER_UI section 8c.-1), so a
        // refresh can put a different server at index N -- an index-keyed selection would
        // silently move to whatever landed there, which is how a player joins a server
        // they did not pick.
        if (rp.bg)
            U::SetImageTint(rp.bg, (!g_selectedId.empty() && r.lobbyId == g_selectedId)
                                       ? kRowSel : kRowBg);
        // The id is captured HERE, in the same pass as the text above it, so a click
        // resolves to the server the user was LOOKING at even if the master reorders.
        g_rowIds[static_cast<size_t>(i)] = r.lobbyId;
    }
    if (g_status) {
        const std::string s = sm::Status();
        const std::wstring w(s.begin(), s.end());
        E::SetWidgetText(g_status, w.c_str());
    }
}

// Build the screen once per menu instance. FAIL-CLOSED: a null donor means DO NOT BUILD
// and retry, never fall back to a default style -- that fallback is the Roboto/centred/
// white bug. After enough attempts the user is TOLD, because a silent forever-retry is the
// same defect one level quieter.
bool BuildScreen(void* switcher) {
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
    if (void* cw = R::FindClass(P::name::ContentWidgetClass)) {
        if (void* fn = R::FindFunction(cw, P::name::SetContentFn)) {
            ue_wrap::ParamFrame f(fn);
            f.Set<void*>(L"Content", winOvl);
            Call(winBox, f);
        }
    }
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
            g_title = AddText(titleRow, L"Multivoid  -  Server Browser", 24, kText,
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
    // Column headers are a SECTION HEADER, and those are orange in every native window.
    void* head = Spawn(L"HorizontalBox", col);
    if (head) {
        for (const Column& c : kColumns) AddText(head, c.title, 16, kAccent, kJustLeft, c.weight);
        U::AddChild(col, head);
    }
    void* listBox = Spawn(L"SizeBox", col);
    g_list = listBox ? Spawn(L"ScrollBox", listBox) : nullptr;
    if (!listBox || !g_list) return false;
    U::SetSizeBoxHeight(listBox, kListH);
    // The settings list's scrollbar treatment (section 7b): a server list is the long-list
    // case, and ui_saveSlots' own ScrollBox sets no bar style at all. NINE brushes.
    U::CloneStyle(g_list, P::off::UScrollBox_WidgetBarStyle, barDonor,
                  P::off::UScrollBox_WidgetBarStyle, P::off::FScrollBarStyle_Size,
                  P::off::FScrollBarStyleBrushes, 9);
    if (void* cw = R::FindClass(P::name::ContentWidgetClass)) {
        if (void* fn = R::FindFunction(cw, P::name::SetContentFn)) {
            ue_wrap::ParamFrame f(fn);
            f.Set<void*>(L"Content", g_list);
            Call(listBox, f);
        }
    }
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
    g_rowIds.clear();
    UE_LOGI("server_browser_native: screen built (root=%p list=%p) after %d attempt(s)",
            root, g_list, g_buildAttempts + 1);
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

bool IsOpen() { return g_shown; }


void OnMenuTick(void* menu, void* switcher) {
    if (!Armed() || !menu || !switcher) return;
    g_switcher = switcher;

    // Rebuild on a new menu instance (the old widgets died with it).
    if (menu != g_menu) {
        g_menu = menu;
        g_root = nullptr; g_list = nullptr; g_status = nullptr; g_title = nullptr;
        g_closeBtn = nullptr; g_backBtn = nullptr; g_scrimW = nullptr;
        g_ourIndex = -1; g_shown = false; g_buildAttempts = 0; g_toldTheUser = false;
        g_rowIds.clear();
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
    selftest::Tick(g_scrimW, g_list, g_closeBtn);

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
        const bool wasPrimed = g_escPrimed;
        const bool prevEsc   = g_prevEsc;
        if (!g_escPrimed) { g_escPrimed = true; g_prevEsc = esc; }
        const bool pressEdge = esc && !g_prevEsc;
        // ONE-SHOT DIAGNOSTIC. ESC has not closed this screen in any run on 2026-08-26,
        // while the selftest's own GetAsyncKeyState reads the key DOWN at the same moment
        // -- so the key reaches the process and the EDGE is being eaten somewhere in these
        // four lines. Print the inputs the first time the key reads down without producing
        // an edge; that names which of prevEsc / primed did it, which no amount of reading
        // the code has settled.
        static bool loggedEatenEdge = false;
        if (esc && !pressEdge && !loggedEatenEdge) {
            loggedEatenEdge = true;
            UE_LOGW("server_browser_native: ESC reads DOWN but produced NO press edge "
                    "(prevEsc=%d wasPrimed=%d shown=%d) -- this is why the screen did not "
                    "close", prevEsc ? 1 : 0, wasPrimed ? 1 : 0, g_shown ? 1 : 0);
        }
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
            // A click on a hovered row SELECTS it. The row under the cursor is already
            // known from the hover pass, so this costs no extra dispatch.
            if (g_hoverRow >= 0 && g_hoverRow < static_cast<int>(g_rowIds.size())) {
                const std::string& id = g_rowIds[static_cast<size_t>(g_hoverRow)];
                if (!id.empty() && id != g_selectedId) {
                    g_selectedId = id;
                    RepaintRowFills();
                    UE_LOGI("server_browser_native: row selected (%s)", id.c_str());
                }
            }
        }
    }

    UpdateHover();

    const uint64_t now = ::GetTickCount64();
    if (now - g_lastRefreshMs >= kRefreshMs) {
        g_lastRefreshMs = now;
        sm::Refresh();
        SyncRows();
    }
}

}  // namespace ui::server_browser_native
