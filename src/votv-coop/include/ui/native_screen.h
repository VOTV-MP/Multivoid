// ui/native_screen.h -- the construction kit every NATIVE (UMG) coop screen is built from.
//
// WHY IT EXISTS. `server_browser_native.cpp` proved the whole approach -- a hand-built
// `UUserWidget` added as a child of `ui_menu_C::switcher_widgets`, reflection only, no
// Blueprint, no editor, no pak -- and in doing so it accumulated the primitives ANY such
// screen needs: the measured palette, a framed box, a styled text block, a real UButton
// cloned from a game donor. The hosting window is the second such screen, and copying
// those primitives into it would be the exact failure this project has already paid for
// twice in one day: two modules reading one directory on opposite assumptions
// (mod_environment vs skin_registry), and a sub-screen donor read the wrong way producing
// a false finding. One owner, or they drift.
//
// WHAT BELONGS HERE: anything a SECOND native screen would need verbatim. What does NOT:
// the browser's row model, its column table, its hover/selection state -- those are the
// browser, not the kit.
//
// THE FACTS THE KIT ENCODES, all measured (docs/MULTIPLAYER_UI.md section 8a, and
// docs/VOTV_UI_STYLE.md for the palette). Each is a trap someone already fell into:
//
//   * a donor MUST be read off the switcher's own CHILD. `R::FindObjectByClass` returns a
//     DIFFERENT non-CDO instance (a WidgetBlueprint's widget-tree template is not named
//     `Default__`, so reflection.cpp's CDO skip does not exclude it) and every field read
//     through it comes back null -- a false finding on the probe's first run;
//   * a `UImage` with a TINT and no ResourceObject draws a SOLID RECT, which is how the
//     game's own full-screen scrim works -- so frames and fills need no donor and no art;
//   * `ESlateVisibility` 2 is HIDDEN, not "chrome". Writing 2 for "draws but never eats a
//     click" made the frame and panel fill not draw AT ALL; 3 (HitTestInvisible) is the
//     value that means it;
//   * the palette is sRGB and `FLinearColor` is LINEAR. Writing the byte value straight in
//     renders #31 as #7B -- the whole palette washed out, unfixable by re-picking values;
//   * a button's STYLE is cloned (that is what carries the game's press/hover sounds) but
//     its LABEL is authored. Cloning a donor `UTextBlock`'s style was tried on the menu
//     inject and REVERTED: the donor reads null at some timings and the silent fallback is
//     Roboto/centred/white.
//
// THREADING: game thread only. Every function here spawns UObjects or calls UFunctions.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace ui::native_screen {

using ue_wrap::FLinearColor;

// ---- alignment enums, spelled out so call sites read as prose -----------------------
// EHorizontalAlignment / EVerticalAlignment: Fill=0 Left=1/Top=1 Center=2 Right=3/Bottom=3.
inline constexpr uint8_t kFill = 0, kLeft = 1, kCenter = 2;
// ETextJustify: Left=0 Center=1 Right=2.
inline constexpr uint8_t kJustLeft = 0, kJustCenter = 1;

// ---- the palette -------------------------------------------------------------------
// docs/VOTV_UI_STYLE.md section 2. Every value is SAMPLED from the game's own menus by
// histogram, and the set is a designed ramp rather than an accumulation -- #1A1A1A /
// #313131 / #404040 step evenly, and #400040 (selected) and #400000 (destructive) are one
// 0x40 component moved between channels. A colour that is not in that table is a mistake.
//
// Functions, not constants: the sRGB->linear conversion is not constexpr-friendly
// (std::pow), and a header-inline `const FLinearColor` would mint one copy per TU.
FLinearColor Srgb(int r, int g, int b, float a = 1.f);

FLinearColor Panel();    // #1A1A1A window fill
FLinearColor Border();   // #646464 -- every frame in the game's menus
FLinearColor RowBg();    // #313131 a list row at rest
FLinearColor RowSel();   // #400040 ...and selected. FILL, not text (style doc section 4)
FLinearColor Text();     // #FFFFFF the default: most text is white
FLinearColor Accent();   // #FF7C00 orange -- the interactive accent
FLinearColor Hover();    // #FFFF00 hover is a TEXT colour, never a fill
FLinearColor Amber();    // #FFBC00 value emphasis; the mismatch tint
FLinearColor Dim();      // #A5A5A5 secondary text (measured, not guessed)
FLinearColor Own();      // #9EEAB3 "your server"

// ---- primitives ---------------------------------------------------------------------

// Read a pointer field at a byte offset. Returns null for a null base or a negative
// offset, so an unresolved offset degrades to "absent" instead of dereferencing garbage.
void* ReadPtr(void* base, int32_t off);

// Find the switcher's own child of a class -- THE way to obtain a donor (see above).
void* SwitcherChild(void* switcher, const wchar_t* className);

// Read a UPROPERTY pointer field by NAME off a live object.
void* DonorField(void* owner, const wchar_t* field);

// NewObject<cls>(outer). Null if the class does not resolve.
void* Spawn(const wchar_t* cls, void* outer);

// One styled UTextBlock, added to `panel`. `fillWeight` > 0 makes it a filling
// horizontal-box slot (clipped, with a right gutter); 0 leaves the slot auto-sized.
void* AddText(void* panel, const wchar_t* initial, int32_t size, const FLinearColor& col,
              uint8_t justify, float fillWeight);

// THE FRAME: an outer bordered UImage plus an inner fill inset by `borderPx`. Every panel,
// row, header strip and value cell in VOTV's menus is a bordered box with sharp corners
// and nothing in the game's UI floats unboxed. Returns the OVERLAY to put content in.
void* AddFramedBox(void* parent, const FLinearColor& fill, float borderPx);

// A chrome UButton with an authored text label, styled from a donor UButton -- a REAL
// UButton because that is what carries the game's press and hover sounds.
// THE DONOR'S OWN SIZE, MEASURED, not a number that looked right. The game gives
// `button_back`'s label font_ui at size 20 (native_screen[fontprobe], 2026-08-30);
// ours were 18. Two points on a monospace pixel face is exactly the "different
// font" a player sees -- which, with the ALL-CAPS labels that shipped beside it,
// is what the 2026-08-30 report was describing. Same asset, same outline, wrong
// size and wrong case.
inline constexpr int32_t kBtnFontPx = 20;

void* BuildButton(void* parent, void* donorBtn, const wchar_t* label, int32_t fontSize);

// WHICH CHILD OF `panel` IS UNDER THE CURSOR, by GEOMETRY. -1 for none.
//
// This exists because asking Slate does not work here, and the measurement is worth
// carrying with the function: a row background is a `UImage` with Visibility=Visible whose
// rect CONTAINS the cursor, with every link in its parent chain Visible or
// SelfHitTestInvisible -- and `UWidget::IsHovered()` on it reads **0** when it sits inside
// a `UScrollBox`. A `UButton` outside the ScrollBox in the same screen and the same tick
// reads 1. Measured 2026-08-29 on the server browser, whose row hover and row SELECTION had
// therefore never worked; the hosting window shipped the same day with the same construct.
// Both call this now, which is the only reason it lives in the shared kit rather than in
// one of them.
//
// `count` is how many children are actually SHOWN, not `ChildCount`: screens here grow rows
// and never remove them (a surplus row is Collapsed), so ChildCount is a high-water mark
// and a collapsed widget keeps the rect it last painted with -- which can still contain the
// cursor and win. `hint` is the previously-hovered index, probed first: during a sweep the
// pointer is on the same row for most frames, so the common case costs ONE rect read.
//
// The walk is UNORDERED and complete. It used to stop at the first child beginning below
// the cursor, on the reasoning that children are stacked top to bottom -- but a scrolled-out
// child is not arranged and keeps a stale rect, and the rows are rebuilt on every sync, so
// one out-of-order child ended the walk and returned "no row" for the whole list. The cost
// of dropping it is at most `count` rect reads on a list bounded by kMaxRows, on a poll that
// runs only when the pointer or the scroll moved.
//
// `cx`/`cy` ARE DESKTOP PIXELS -- the space `GetCursorPos` reports, unconverted.
//
// THIS COMMENT SAID THE OPPOSITE FOR ONE DAY AND IT COST THREE RUNS. It claimed client
// pixels, on the theory that `LocalToAbsolute` reports in client space; the harness was
// changed to match, which double-counted the client origin and aimed every cursor a whole
// origin low. `measured` 2026-08-30 from the probe's own child table: with the client area
// at desktop (320,180), the list panel reports (796,496) and its live rows span desktop
// y 496..752 -- the same numbers `GetCursorPos` returns for a pointer on them. Absolute IS
// desktop here. If a future build introduces a viewport UI scale, do not re-derive this by
// hand a third time: call `umg::CursorToWidgetAbsolute`, which is Slate's own inverse, and
// let both sides of the comparison come from one source.
int32_t ChildAtCursor(void* panel, int32_t count, long cx, long cy, int32_t hint = -1);

// IS THE POINTER OVER THIS ONE WIDGET -- by GEOMETRY, never by `IsHovered()`.
//
// The engine's own `IsHovered()` is not usable on the widgets we build: measured 0 across
// this tree on 2026-08-29 and again on 2026-08-30, on a UImage set Visible whose reported
// rect plainly contains the cursor, both inside a ScrollBox and outside one. Screens that
// asked it got a control that never lit and never clicked. Real `UButton`s are exempt --
// Slate delivers their clicks itself -- but every hand-built row, tile or image target must
// come through here so there is ONE hit-test mechanism in the native screens rather than
// two that disagree.
bool CursorOverWidget(void* w);

// THE HIT TEST PLUS THE THING THAT SAYS WHEN TO REDO IT -- one object, because shipping
// them apart is a defect this project has now made twice in one day.
//
// `ChildAtCursor` above was extracted so two screens could share the hit test. The sharing
// stopped there, and everything that makes the hit test CORRECT stayed duplicated per
// screen: the settling pass, the scroll term, the count of rows that are actually shown.
// Only one copy got fixed. The other kept a cursor-motion-only gate over a scrollable list,
// which means the wheel slides a row out from under a stationary pointer while the stored
// index stays put -- and on that screen the stored index chooses which WORLD to load.
// Same bug twice is the level being wrong (docs/LESSONS.md), so the level moved here.
//
// Hold one per scrolling list. `Poll` returns true on a tick the caller should act on --
// the pointer moved, the list scrolled, the row count changed, or it is the settling tick
// owed after motion stops (Slate's hover reads one tick behind the pointer, so evaluating
// only on the moving tick leaves the answer permanently one move stale). A tick where
// nothing could have changed costs one dispatch and returns false.
class HoverTracker {
public:
    // `shownCount` MUST be the number of children actually displayed. Passing ChildCount is
    // the documented mistake: rows are grown and never removed, so it is a high-water mark,
    // and a collapsed child keeps the rect it last painted with.
    bool Poll(void* panel, int32_t shownCount);

    // The child under the cursor, or -1. Valid after Poll.
    int32_t Index() const { return index_; }

    // Forget everything. Call when the screen is SHOWN and when its widgets are rebuilt:
    // the pointer has not moved, so nothing else would re-evaluate, and a hover index
    // remembered across a hide/show points into a list that may have been rebuilt under it.
    void Reset();

private:
    long    lastX_ = -1, lastY_ = -1;
    float   lastFrac_  = -2.f;   // -1 is a legitimate 'unreadable'; the sentinel must differ
    int32_t lastCount_ = -1;
    int32_t index_     = -1;
    bool    pending_   = false;
};

}  // namespace ui::native_screen
