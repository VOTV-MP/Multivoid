// ui/native_screen.h -- the construction kit every native (UMG) coop screen is built from: a
// hand-built UUserWidget added as a child of ui_menu_C::switcher_widgets, reflection only, no
// Blueprint, no editor, no pak. It holds what a second native screen needs as is (the measured
// palette, the framed box, the styled text block, the window shell, the donor-cloned button, the
// hit test); a screen's own model, columns and selection state stay in the screen. One owner, or
// the copies drift. The measurements are in docs/VOTV_UI_STYLE.md. Game thread only: every
// function spawns UObjects or calls UFunctions.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>

namespace ui::native_screen {

using ue_wrap::FLinearColor;

// The rendered width of one frame ring, in Slate units: the game's 9-slice inst_uiBorder draws a
// 2 px light band and a 2 px dark one per edge. Every distance that lines a nested frame up with
// its parent's is this, not the 2 px the flat fallback insets its fill by; spelling both as 2.f
// is how a second ring once merged into the first.
inline constexpr float kNativeRingPx = 4.f;

// EHorizontalAlignment / EVerticalAlignment: Fill=0, Left=1 / Top=1, Center=2, Right=3 / Bottom=3.
inline constexpr uint8_t kFill = 0, kLeft = 1, kCenter = 2, kRight = 3;
inline constexpr uint8_t kTop = 1, kBottom = 3;
// ETextJustify: Left=0, Center=1, Right=2.
inline constexpr uint8_t kJustLeft = 0, kJustCenter = 1, kJustRight = 2;

// The palette, sampled from the game's own menus by histogram (docs/VOTV_UI_STYLE.md): a designed
// ramp (#1A1A1A, #313131, #404040 step evenly; the selected fill moves one 0x40 component between
// channels), so a colour outside the table is a mistake. Functions, not constants: the
// sRGB-to-linear conversion is not constexpr, and a header-inline const would mint a copy per
// TU.
FLinearColor Srgb(int r, int g, int b, float a = 1.f);

FLinearColor Panel();    // #1A1A1A window fill
FLinearColor Border();   // #646464 -- every frame in the game's menus
FLinearColor RowBg();    // #313131 a list row at rest
FLinearColor RowSel();   // #400040 selected: the fill, never the text
FLinearColor Text();     // #FFFFFF the default: most text is white
FLinearColor Accent();   // #FF7C00 orange -- the interactive accent
FLinearColor Hover();    // #FFFF00 hover is a TEXT colour, never a fill
FLinearColor Amber();    // #FFBC00 value emphasis; the mismatch tint
FLinearColor Dim();      // #A5A5A5 secondary text (measured, not guessed)
FLinearColor Own();      // #9EEAB3 "your server"
FLinearColor Bad();      // #FF0000 the game's own destructive and out-of-date red; the version mismatch
FLinearColor Black();    // #000000 the status pane's fill, the one place the game goes black behind text

// A pointer field at a byte offset; null for a null base or a negative offset, so an unresolved
// offset reads as absent.
void* ReadPtr(void* base, int32_t off);

// The switcher's own child of a class: the way to obtain a donor (FindObjectByClass returns a
// different non-CDO instance whose fields read null).
void* SwitcherChild(void* switcher, const wchar_t* className);

// A UPROPERTY pointer field by name off a live object.
void* DonorField(void* owner, const wchar_t* field);

// A widget inside a live UUserWidget's tree by name, for a donor that may not be a designer
// variable (a UMG widget gets a UPROPERTY only with "Is Variable" ticked; the frame donor
// ui_settings.image_border has it false, so a field read is null forever). One GUObjectArray
// walk: resolve once per menu instance and latch it (BorderDonorResolved), never per tick.
void* DonorChild(void* userWidget, const wchar_t* name);

// NewObject<cls>(outer); null if the class does not resolve.
void* Spawn(const wchar_t* cls, void* outer);

// One styled UTextBlock in `panel`; fillWeight > 0 makes it a filling horizontal-box slot
// (clipped, with a right gutter), 0 leaves the slot auto-sized.
void* AddText(void* panel, const wchar_t* initial, int32_t size, const FLinearColor& col,
              uint8_t justify, float fillWeight);

// A child added to a box with a fill weight on its slot; returns the slot, or null. Weight 0 is
// ESlateSizeRule::Automatic, above 0 is Fill. Two functions, not one with an offset parameter:
// the horizontal and vertical slot layouts are different structures, and a write through the
// wrong one lands in a neighbouring field and fails nowhere near the call.
void* AddHFill(void* hbox, void* child, float weight, uint8_t h, uint8_t v);
void* AddVFill(void* vbox, void* child, float weight, uint8_t h, uint8_t v);

// The same write on an existing slot: BuildButton attaches and centres its own button, so a
// caller wanting it to fill a grid cell reconfigures the slot.
void SetHSlot(void* slot, float weight, uint8_t h, uint8_t v);
void SetVSlot(void* slot, float weight, uint8_t h, uint8_t v);

// The slot a widget occupies (UWidget::Slot), or null when unattached.
void* SlotOf(void* widget);

// A slot's FMargin padding {left, top, right, bottom}; padOff is the slot type's own Padding
// offset, per box type for the reason above.
void SetSlotPadding(void* slot, size_t padOff, float l, float t, float r, float b);

// The frame: a fill and a border image in one overlay, since every panel, row, header strip and
// value cell in VOTV's menus is a bordered box with sharp corners. With a donor the border is the
// game's own 9-slice material painted over the fill at full size; without one, a flat rectangle
// with the fill inset by borderPx. Returns the overlay to put content in. One ring per box: the
// ladder of bands on a native window is nesting (a panel's ring flush against its parent's), so
// nest boxes with zero content padding, never double a border.
//
// The frame donor is a live ui_settings_C.image_border, whose FSlateBrush every framed box
// clones; set it once per screen build, before the first AddFramedBox. A colour could never
// match: each native edge carries its own pair of 2 px bands (top and bottom #A5A5A5 to #585858,
// left and right #919191 to #646464, a bevel lit from above), which is the material inst_uiBorder
// as a 9-slice box with margin 0.5. Null is tolerated and degrades to the flat fill: a frame is
// cosmetic, unlike the donors that decide whether a screen is usable.
void SetBorderDonor(void* donorImage);

// Drops the cached donor on a menu-instance edge: it belongs to that menu's ui_settings, and
// carried across a rebuild CloneStyle would read a destroyed widget.
void ForgetBorderDonor();

// Whether SetBorderDonor ran for this menu instance, whatever the outcome, so a builder resolves
// the donor once rather than walking the array on every retry tick.
bool BorderDonorResolved();

void* AddFramedBox(void* parent, const FLinearColor& fill, float borderPx);

// The framed box's parts, resolved by the kit. AddFramedBox's child order differs by frame (flat
// {edge, face, content}, framed {face, edge, content}), and a caller reading the slots by index
// once read a UImage as a panel: UPanelWidget::Slots and UImage::Brush sit at the same offset,
// so GetChildAt read the brush's vtable pointer as the slot array, an absorbed AV or every row
// blank. The border is found by reading it (a cloned frame carries the material in its brush's
// ResourceObject; a tinted fill carries nothing), never by counting. `content` is the child the
// caller added after the box was built, null if none. False when the overlay is not a framed box.
struct FramedParts {
    void* edge    = nullptr;  // the border image (the 9-slice ring when a donor was cloned)
    void* face    = nullptr;  // the fill image
    void* content = nullptr;  // whatever the caller put in
};
bool FramedBoxParts(void* overlay, FramedParts& out);

// A chrome UButton with an authored label, styled from a donor UButton: a real UButton, since
// that is what carries the game's press and hover sounds. The label size is the donor's own,
// measured: the game gives button_back's label font_ui at 20, and at 18 a pixel face reads as a
// different font.
inline constexpr int32_t kBtnFontPx = 20;

void* BuildButton(void* parent, void* donorBtn, const wchar_t* label, int32_t fontSize);

// The window shell: everything a native sub-screen is before its content, the UUserWidget for
// the menu's switcher, its widget tree, the full-screen scrim, the centred framed window, the
// title strip and the content column the caller fills. It builds and centres the window but
// does not attach `root` to the switcher: attaching is where a screen learns its own index, and
// a wrong one names one of the game's screens (clicking MULTIPLAYER once opened VOTV's Stats
// panel), so that step stays at each call site beside its index check.
struct WindowShell {
    void* root   = nullptr;   // the UUserWidget -- what the caller adds to the switcher
    void* scrim  = nullptr;   // the full-screen dim; also what absorbs a stray click
    void* column = nullptr;   // the UVerticalBox under the title, for the caller's content
    // The frame itself, the centred SizeBox at widthPx x heightPx. `root` is full-screen (the scrim
    // fills it), so a fit check against it reports slack while a footer sits outside the ring; "is
    // my content inside the frame" needs this rect.
    void* box    = nullptr;
};

// False if any widget could not be spawned: a build failure to retry, never a window without a
// scrim. `title` may be null for a window with no title strip.
bool BuildWindowShell(void* switcher, float widthPx, float heightPx, const wchar_t* title,
                      WindowShell& out);

// Which child of `panel` is under the cursor, by geometry; -1 for none. Slate's own answer is
// unusable here: UWidget::IsHovered reads 0 on a Visible row image inside a UScrollBox whose
// rect contains the cursor, while a UButton outside the box reads 1 in the same tick. `count`
// is the number of children shown, not ChildCount: screens grow rows and collapse the surplus,
// and a collapsed widget keeps the rect it last painted with. `hint` is the previous index,
// probed first, so a sweep along one row costs one rect read. The walk is unordered and
// complete: a scrolled-out child keeps a stale rect and the rows are rebuilt on every sync, so
// an early break returned no row for the whole list. `cx`/`cy` are in the space WidgetScreenRect
// reports in, from CursorInWidgetSpace; never raw GetCursorPos pixels.
int32_t ChildAtCursor(void* panel, int32_t count, long cx, long cy, int32_t hint = -1);

// The index a screen returns to when it hides, one rule for the four screens in the switcher.
// `live` is what the switcher reads at Show; it is the answer except when it is our own index
// (a sibling can hand the index back before an open intent is consumed, so a screen can open
// with its own index active; recording itself makes Hide move nothing, and the MULTIPLAYER
// button that would rescue the player sits on an inactive switcher child, a dead end, and once
// the windows reconcile themselves open from the live index, an unclosable one) or negative
// (SwitcherIndex answers -1 on a failed dispatch, and one bad frame would poison the return
// for the menu's life). Keeping `previous` in both cases is still correct or still -1, and Hide
// declines to write a negative.
int32_t SafePriorIndex(int32_t live, int32_t ourIndex, int32_t previous);

// The switcher's active index, read once per menu tick: umg::SwitcherIndex is a ParamFrame
// allocation plus a ProcessEvent dispatch, and four screens compare the live index against
// their own in both directions at the menu's ~117 Hz. BeginMenuTick is called once by the menu
// observer that drives all four; ActiveIndex is valid only inside that tick and answers -1
// before the first call or on a failed read, which every consumer treats as "not ours".
void    BeginMenuTick(void* switcher);
int32_t ActiveIndex();

// Where the cursor is, in the space WidgetScreenRect reports in: the one conversion both hit
// tests go through (GetCursorPos gives desktop pixels; between them and Slate's absolute space
// sit the window's client origin and the viewport's UI scale). False means Slate's inverse would
// not resolve, a refusal: the caller reports no hit and never falls back to another space.
bool CursorInWidgetSpace(long& outX, long& outY);

// Whether the pointer is over one widget, by geometry: IsHovered reads 0 across this tree on a
// Visible UImage whose rect contains the cursor, inside a ScrollBox and outside one. A real
// UButton is exempt (Slate delivers its clicks); every hand-built row, tile or image target
// comes through here, so there is one hit-test mechanism.
bool CursorOverWidget(void* w);

// The same test with the cursor already resolved, for a sweep over several widgets: the
// conversion is the expensive half (GetWorldContext reaches FindObjectByClass, an uncached
// GUObjectArray walk, plus a dispatch and an allocation), and N calls of CursorOverWidget pay N
// walks for one cursor. Resolve once with CursorInWidgetSpace, then this per widget. The
// coordinates must come from that conversion; raw desktop pixels are a second space that agrees
// only at scale 1.
bool WidgetContains(void* w, long hx, long hy);

// The hit test and the rule for when to redo it, in one object: shared apart, only one screen's
// copy kept the settling pass, the scroll term and the shown count, and on the other the wheel
// slid a row out from under a still pointer while the stored index (which chose the world to
// load) stayed put. One per scrolling list. Poll returns true on a tick to act on: the pointer
// moved, the list scrolled, the row count changed, or the settling tick owed after motion stops
// (Slate's hover reads one tick behind). An idle tick costs one dispatch and returns false.
class HoverTracker {
public:
    // shownCount is the number of children displayed, not ChildCount (a high-water mark whose
    // collapsed rows keep their last rect).
    bool Poll(void* panel, int32_t shownCount);

    // The child under the cursor, or -1; valid after Poll.
    int32_t Index() const { return index_; }

    // Forgets everything; called when the screen is shown and when its widgets are rebuilt, since
    // the pointer has not moved and nothing else would re-evaluate a list rebuilt under a
    // remembered index.
    void Reset();

private:
    long    lastX_ = -1, lastY_ = -1;
    float   lastFrac_  = -2.f;   // -1 is a legitimate 'unreadable'; the sentinel must differ
    int32_t lastCount_ = -1;
    int32_t index_     = -1;
    bool    pending_   = false;
};

}  // namespace ui::native_screen
