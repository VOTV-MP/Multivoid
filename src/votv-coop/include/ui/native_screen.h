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

#include <cstddef>
#include <cstdint>

namespace ui::native_screen {

using ue_wrap::FLinearColor;

// THE RENDERED WIDTH OF ONE FRAME RING, in Slate units.
//
// `[V]` the game's 9-slice `inst_uiBorder` draws a 2 px light band and a 2 px dark one per
// edge, measured on both a native capture and our own. Every distance that has to line a
// nested frame up with its parent's is THIS, not the 2 px `borderPx` the flat fallback insets
// its fill by -- those are different quantities and spelling both `2.f` at different call
// sites is how the second ring merged into the first (`919191x4`, a light run the game never
// produces).
inline constexpr float kNativeRingPx = 4.f;

// ---- alignment enums, spelled out so call sites read as prose -----------------------
// EHorizontalAlignment / EVerticalAlignment: Fill=0 Left=1/Top=1 Center=2 Right=3/Bottom=3.
inline constexpr uint8_t kFill = 0, kLeft = 1, kCenter = 2, kRight = 3;
inline constexpr uint8_t kTop = 1, kBottom = 3;
// ETextJustify: Left=0 Center=1 Right=2.
inline constexpr uint8_t kJustLeft = 0, kJustCenter = 1, kJustRight = 2;

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
FLinearColor Bad();      // #FF0000 the game's own destructive/old-version red (style doc
                         // section 2: "Delete save slot", "Hard reset", and the red `0.7.0!`
                         // an out-of-date save shows -- which is exactly our version mismatch)
FLinearColor Black();    // #000000 -- the status pane's fill; the ONE place the game goes
                         // fully black behind text (the save browser's bottom-right pane)

// ---- primitives ---------------------------------------------------------------------

// Read a pointer field at a byte offset. Returns null for a null base or a negative
// offset, so an unresolved offset degrades to "absent" instead of dereferencing garbage.
void* ReadPtr(void* base, int32_t off);

// Find the switcher's own child of a class -- THE way to obtain a donor (see above).
void* SwitcherChild(void* switcher, const wchar_t* className);

// Read a UPROPERTY pointer field by NAME off a live object.
void* DonorField(void* owner, const wchar_t* field);

// Find a widget inside a live UUserWidget's tree BY NAME. Use this instead of DonorField
// whenever the donor might not be a designer VARIABLE: a UMG widget only gets a UPROPERTY when
// "Is Variable" is ticked, and `[V]` the frame donor `ui_settings.image_border` has
// bIsVariable = False, so a field read returns null forever -- silently.
//
// COSTS ONE GUObjectArray WALK. Resolve once per menu instance and latch it; do NOT call this
// from a per-tick retry (see BorderDonorResolved).
void* DonorChild(void* userWidget, const wchar_t* name);

// NewObject<cls>(outer). Null if the class does not resolve.
void* Spawn(const wchar_t* cls, void* outer);

// One styled UTextBlock, added to `panel`. `fillWeight` > 0 makes it a filling
// horizontal-box slot (clipped, with a right gutter); 0 leaves the slot auto-sized.
void* AddText(void* panel, const wchar_t* initial, int32_t size, const FLinearColor& col,
              uint8_t justify, float fillWeight);

// ADD A CHILD TO A BOX AND GIVE ITS SLOT A FILL WEIGHT. Returns the slot, or null.
//
// These exist because a two-COLUMN screen cannot be built without them and every call site
// would otherwise repeat the same four raw writes against `FSlateChildSize` -- the same
// shape `AddText` already carries privately for its own weighted cells. A weight of 0 means
// ESlateSizeRule::Automatic (the child's desired size); anything above 0 is Fill.
//
// The two are separate functions rather than one with an offset parameter because the
// horizontal and vertical slot layouts are DIFFERENT structures that merely happen to agree
// on three of four member offsets today (Size@0x38 vs 0x50). Passing the wrong one writes a
// float into a neighbouring field and the failure appears nowhere near the call
// (docs/LESSONS.md: a wrong-offset write never faults where you wrote it).
void* AddHFill(void* hbox, void* child, float weight, uint8_t h, uint8_t v);
void* AddVFill(void* vbox, void* child, float weight, uint8_t h, uint8_t v);

// The same layout write on a slot that ALREADY EXISTS. `BuildButton` attaches its own
// button (and centres it), so a caller who wants that button to FILL its grid cell has to
// reconfigure the slot rather than create one.
void SetHSlot(void* slot, float weight, uint8_t h, uint8_t v);
void SetVSlot(void* slot, float weight, uint8_t h, uint8_t v);

// The slot a widget currently occupies (UWidget::Slot), or null if it is unattached.
void* SlotOf(void* widget);

// Set a slot's FMargin padding {left, top, right, bottom}. `padOff` is the slot type's own
// Padding offset -- again per box type, for the reason above.
void SetSlotPadding(void* slot, size_t padOff, float l, float t, float r, float b);

// THE FRAME: a fill and a border image in one overlay. Every panel, row, header strip and
// value cell in VOTV's menus is a bordered box with sharp corners and nothing in the game's UI
// floats unboxed. With a donor the border is the game's own 9-slice material painted OVER the
// fill at full size; without one it degrades to a flat rectangle with the fill inset by
// `borderPx`. Returns the OVERLAY to put content in.
//
// ONE RING PER BOX. The ladder of bands the user asked for ("много скосов") is NESTING -- a
// panel's ring sitting flush against its parent's -- not a box wearing two borders. `[V]` the
// native Keybinds window's left edge samples the pair ONCE across its title strip and TWICE
// across its list: same window, two heights, so the second pair belongs to the inner panel.
// Nest boxes and give the parent zero content padding; never double a border.
// THE FRAME DONOR -- a live `ui_settings_C.image_border`, whose FSlateBrush every framed box
// clones. Set it once per screen build, BEFORE the first AddFramedBox call.
//
// WHY A DONOR AND NOT A COLOUR. Our frames were a flat 2 px rectangle in one grey, and the
// user rejected them as not VOTV's frames. Sampling the native window proves them right, and
// says why a colour could never have worked: each edge carries its OWN pair of 2 px bands, and
// the horizontal pair is not the vertical one --
//     top / bottom  #A5A5A5 -> #585858        left / right  #919191 -> #646464
// -- a raised bevel lit from above. (Measured on
// `ignore_folder/votv_widgets_style/SERVER_BROWSER_4_keybinds_tab_opens_new_window.png`, the
// 1885x1046 capture. An earlier set of eight darker values in this comment came from a
// dimmed/downscaled capture and did not survive re-sampling at full resolution.) That is not a rectangle outline in any colour; it
// is `[V]` the MATERIAL `inst_uiBorder` drawn as a 9-slice box with Margin 0.5, which is what
// `ui_settings`'s `image_border` carries. So the fix is to stop inventing a frame and clone
// the game's, exactly as the BACK button already clones `button_back`'s style.
//
// Null is tolerated and degrades to the old flat fill: a frame is cosmetic, and the
// fail-CLOSED rule that governs the browser's other donors exists because THOSE decide
// whether a screen is usable at all. Losing the bevel is not that.
void SetBorderDonor(void* donorImage);

// Drop the cached donor on a MENU-INSTANCE edge. The donor belongs to that menu's
// `ui_settings`; carried across a rebuild it would have CloneStyle read a destroyed widget.
void ForgetBorderDonor();

// Has SetBorderDonor been called for this menu instance (whatever the outcome)? Lets a builder
// resolve the donor ONCE rather than paying a GUObjectArray walk on every retry tick.
bool BorderDonorResolved();

void* AddFramedBox(void* parent, const FLinearColor& fill, float borderPx);

// THE FRAMED BOX'S PARTS, resolved BY THE KIT rather than guessed by the caller.
//
// `AddFramedBox`'s child order is not one thing: flat it is {edge, face, content}, framed it is
// {face, edge, content} -- the two are mirror images because a cloned 9-slice must be painted
// OVER the fill while a solid rectangle must be painted UNDER it or it would cover the box.
// `server_browser_rows.cpp` used to read those slots by literal index, and its own comment said
// what would happen -- *"if that kit function ever reorders them this reads the wrong image"*.
// A short-lived second ring did exactly that, and the failure was not cosmetic: index 2 became
// a UImage, and `[V]` `UPanelWidget::Slots` and `UImage::Brush` sit at the SAME offset 0x108,
// so `GetChildAt` on it reads the brush's vtable pointer as the slot array's data. Either an
// absorbed AV that kills the rest of the menu tick, or every row silently blank.
//
// So the order lives in ONE place -- here, next to the code that creates it -- and it is
// resolved by READING the border (a cloned frame carries the material in its brush's
// ResourceObject; a tinted fill carries nothing), never by counting children. The count is a
// convention two files have to keep agreeing about; the material is a fact about the widget.
// `content` is the child the caller added after the box was built (null if it added none).
// Returns false if the overlay does not look like a framed box at all.
struct FramedParts {
    void* edge    = nullptr;  // the border image (the 9-slice ring when a donor was cloned)
    void* face    = nullptr;  // the fill image
    void* content = nullptr;  // whatever the caller put in
};
bool FramedBoxParts(void* overlay, FramedParts& out);

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

// ---- the window shell ----------------------------------------------------------------
//
// EVERYTHING A NATIVE SUB-SCREEN IS BEFORE ITS CONTENT: the `UUserWidget` that goes in the
// menu's switcher, its widget tree, the full-screen scrim, the centred framed window, the
// title strip, and the content column the caller fills.
//
// WHY IT IS HERE NOW AND WAS NOT BEFORE. `OPUS_48_DISCIPLINE.md:196-197` -- no new
// framework before N>=3 working cases -- and this is the third: the server browser, the
// hosting window, and the browser's input screens. The first two carry byte-identical
// copies of these sixty lines including the comments, and every one of the traps those
// comments record (a donor read off the switcher's own child; `ESlateVisibility` 2 being
// HIDDEN; a tinted `UImage` with no ResourceObject BEING the scrim; the widget-tree
// back-pointers that must be written by hand) is a trap each copy could stop agreeing
// about independently.
//
// It builds and CENTRES the window but deliberately does NOT attach `root` to the
// switcher: attaching is where a screen learns its own index, and getting that wrong names
// one of the GAME's screens (measured -- clicking MULTIPLAYER opened VOTV's Stats panel).
// That step stays visible at each call site with the index check beside it.
struct WindowShell {
    void* root   = nullptr;   // the UUserWidget -- what the caller adds to the switcher
    void* scrim  = nullptr;   // the full-screen dim; also what absorbs a stray click
    void* column = nullptr;   // the UVerticalBox under the title, for the caller's content
    // THE FRAME ITSELF -- the centred SizeBox at widthPx x heightPx.
    //
    // `root` is a FULL-SCREEN UUserWidget (the scrim fills it), so measuring against `root`
    // answers "how far is this from the edge of the SCREEN", which is not a question anyone
    // here is asking. A fit check written against `root` reports hundreds of pixels of slack
    // while the footer sits outside the ring -- measured 2026-09-01, when exactly that probe
    // returned a false all-clear. These screens are fixed-height with Auto-sized children, so
    // "is my content still inside the frame" is a real question and it needs THIS rect.
    void* box    = nullptr;
};

// False if any widget could not be spawned -- the caller must treat that as a build
// failure and retry, never as "carry on without a scrim". `title` may be null for a
// window with no title strip.
bool BuildWindowShell(void* switcher, float widthPx, float heightPx, const wchar_t* title,
                      WindowShell& out);

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

// WHERE THE CURSOR IS, IN THE SPACE `WidgetScreenRect` REPORTS IN.
//
// The ONE conversion both hit tests go through: `GetCursorPos` gives DESKTOP pixels,
// Slate reports ABSOLUTE, and the terms between them are the window's client origin AND
// the viewport's UI scale. False means Slate's own inverse transform would not resolve --
// which is a REFUSAL, not a miss: the caller must report "no hit" rather than fall back to
// any other space. (It used to fall back to client pixels, silently, which is exactly the
// space a measured correction had already rejected.)
bool CursorInWidgetSpace(long& outX, long& outY);

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

// THE SAME TEST WITH THE CURSOR ALREADY RESOLVED -- for a caller sweeping SEVERAL widgets.
//
// `CursorOverWidget` converts the cursor into widget space on every call, and that
// conversion is the expensive half: it reaches `GetWorldContext()` ->
// `R::FindObjectByClass`, an uncached linear walk of `GUObjectArray`, plus a ProcessEvent
// dispatch and a heap allocation. The rect read is one more dispatch. So an N-widget sweep
// through `CursorOverWidget` pays N array walks to answer a question whose cursor half is
// identical for all N -- the per-frame full-array-scan pattern CLAUDE.md names.
//
// Resolve once with `CursorInWidgetSpace`, then call this per widget: N walks become 1.
// `HoverTracker::Poll` has always done it this way for a LIST; this is the same hoist for
// the screens that hold a handful of NAMED widgets instead. Measured 2026-09-01 by the
// post-ship audit: the hosting window's step-two sweep was 4 walks per menu tick (~468/s).
//
// Coordinates MUST come from `CursorInWidgetSpace` -- passing raw desktop pixels reintroduces
// the two-spaces-that-disagree defect of 2026-08-31, which worked only at scale 1.0.
bool WidgetContains(void* w, long hx, long hy);

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
