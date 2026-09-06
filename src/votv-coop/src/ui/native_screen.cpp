// ui/native_screen.cpp -- the hand-built native-screen kit: the measured palette, the framed box,
// the window shell, the chrome button and the one hit test. See ui/native_screen.h and
// docs/VOTV_UI_STYLE.md for the measurements behind the constants.

#include "ui/native_screen.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"   // the hit-space probe's one line
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>   // GetCursorPos -- HoverTracker reads the real pointer

#include <cmath>
#include <cstdlib>   // the hit probe's raisable cap reads one env var

namespace ui::native_screen {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace U = ue_wrap::umg;
namespace P = ue_wrap::profile;

}  // namespace

// The palette values are sRGB bytes and FLinearColor is linear (the framebuffer converts back on
// the way out): 0x31/255 written as a linear tint lands on screen at about #7B, more than double
// the intended #31, and the whole palette washes out. The same transform as
// FLinearColor::FromSRGBColor.
FLinearColor Srgb(int r, int g, int b, float a) {
    auto f = [](int v) {
        const float c = static_cast<float>(v) / 255.f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return FLinearColor{f(r), f(g), f(b), a};
}

FLinearColor Panel()  { return Srgb(0x1A, 0x1A, 0x1A); }
FLinearColor Border() { return Srgb(0x64, 0x64, 0x64); }
FLinearColor RowBg()  { return Srgb(0x31, 0x31, 0x31); }
FLinearColor RowSel() { return Srgb(0x40, 0x00, 0x40); }
FLinearColor Text()   { return Srgb(0xFF, 0xFF, 0xFF); }
FLinearColor Accent() { return Srgb(0xFF, 0x7C, 0x00); }
FLinearColor Hover()  { return Srgb(0xFF, 0xFF, 0x00); }
FLinearColor Amber()  { return Srgb(0xFF, 0xBC, 0x00); }
FLinearColor Dim()    { return Srgb(0xA5, 0xA5, 0xA5); }
FLinearColor Own()    { return Srgb(0x9E, 0xEA, 0xB3); }
FLinearColor Bad()    { return Srgb(0xFF, 0x00, 0x00); }
FLinearColor Black()  { return Srgb(0x00, 0x00, 0x00); }

void* ReadPtr(void* base, int32_t off) {
    return (base && off >= 0) ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off)
                              : nullptr;
}

// A sub-screen donor is read off the switcher's own child list: FindObjectByClass returns a
// different non-CDO instance (a WidgetBlueprint's widget-tree template is not named Default__, so
// the CDO skip in reflection.cpp keeps it), and every donor field read through it is null.
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

void* DonorChild(void* userWidget, const wchar_t* name) {
    // Not DonorField: a UMG widget has a UPROPERTY only when the designer ticked "Is Variable", and
    // ui_settings.image_border has bIsVariable false (its sibling scrollboxRoot is true, which is
    // why that donor resolves), so a field read returns null forever. The WidgetTree holds every
    // authored widget regardless.
    if (!userWidget) return nullptr;

    // One targeted walk over the tree's children, compared with NameEquals (allocation-free and
    // case-insensitive, since a package can register a name with different casing first).
    // UUserWidget::GetWidgetFromName does not resolve in this build, and ChildObjectsOf would
    // materialise the whole tree with two wstrings per entry per call.
    void* tree = DonorField(userWidget, L"WidgetTree");
    if (!tree) return nullptr;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLiveByIndex(o, i)) continue;
        if (R::OuterOf(o) != tree) continue;
        if (R::NameEquals(R::NameOf(o), name)) return o;
    }
    return nullptr;
}

void* Spawn(const wchar_t* cls, void* outer) {
    void* k = R::FindClass(cls);
    return k ? E::SpawnUObject(k, outer) : nullptr;
}

// One styled UTextBlock in `panel`, with an optional horizontal-box fill weight (0 = auto-size).
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
        // The slot bounds the layout, not the painting: without clipping a long world name paints
        // across the Age column. EWidgetClipping::ClipToBounds = 1.
        U::SetClipping(t, 1);
        // A clipped value touching the next column reads as a rendering fault; a right gutter fixes
        // it. FMargin is {Left, Top, Right, Bottom}.
        auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(slot) +
                                             P::off::UHorizontalBoxSlot_Padding);
        pad[0] = 0.f; pad[1] = 0.f; pad[2] = 18.f; pad[3] = 0.f;
    }
    return t;
}

// The frame: every panel, row, header strip and value cell in VOTV's menus is a bordered box with
// sharp corners, and nothing floats unboxed. Two stacked UImages give that (an outer one with
// the border, an inner one inset by the border width with the fill); a UImage with no
// ResourceObject draws a solid rect, as the game's own scrim does. Returns the overlay the
// caller puts content in; content lands above the fill.
namespace {

// The one child-size write AddHFill and AddVFill share. ESlateSizeRule: Automatic=0, Fill=1.
void WriteChildSize(void* slot, size_t sizeOff, float weight) {
    auto* s = reinterpret_cast<uint8_t*>(slot) + sizeOff;
    *reinterpret_cast<float*>(s + P::off::FSlateChildSize_Value) = weight > 0.f ? weight : 1.f;
    *(s + P::off::FSlateChildSize_SizeRule) = weight > 0.f ? 1 : 0;
}

}  // namespace

void SetHSlot(void* slot, float weight, uint8_t h, uint8_t v) {
    if (!slot) return;
    WriteChildSize(slot, P::off::UHorizontalBoxSlot_Size, weight);
    U::SetSlotAlign(slot, P::off::UHorizontalBoxSlot_HAlign,
                    P::off::UHorizontalBoxSlot_VAlign, h, v);
}

void SetVSlot(void* slot, float weight, uint8_t h, uint8_t v) {
    if (!slot) return;
    WriteChildSize(slot, P::off::UVerticalBoxSlot_Size, weight);
    U::SetSlotAlign(slot, P::off::UVerticalBoxSlot_HAlign,
                    P::off::UVerticalBoxSlot_VAlign, h, v);
}

void* SlotOf(void* widget) {
    return ReadPtr(widget, static_cast<int32_t>(P::off::UWidget_Slot));
}

void* AddHFill(void* hbox, void* child, float weight, uint8_t h, uint8_t v) {
    void* slot = U::AddChild(hbox, child);
    SetHSlot(slot, weight, h, v);
    return slot;
}

void* AddVFill(void* vbox, void* child, float weight, uint8_t h, uint8_t v) {
    void* slot = U::AddChild(vbox, child);
    SetVSlot(slot, weight, h, v);
    return slot;
}

void SetSlotPadding(void* slot, size_t padOff, float l, float t, float r, float b) {
    if (!slot) return;
    auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(slot) + padOff);
    pad[0] = l; pad[1] = t; pad[2] = r; pad[3] = b;
}

bool BuildWindowShell(void* switcher, float widthPx, float heightPx, const wchar_t* title,
                      WindowShell& out) {
    out = WindowShell{};
    if (!switcher) return false;
    constexpr float kBorderPx = 2.f;
    // The window's content is inset by exactly one ring width: on a native window the window's ring
    // and the list panel's ring abut, so the panel is inset by the width the window's ring renders,
    // 4 px on the native capture and on ours. At 6 the panel's ring merged with the window's inner
    // band; at 0 it vanished under the border image, painted last at full size. 4 is also what the
    // game authored (seven of eleven border slots carry a slot offset of 4), and as a slot offset
    // it tracks DPI the way the game's own borders do.
    constexpr float kPadPx    = kNativeRingPx;

    void* root = Spawn(P::name::UserWidgetClass, switcher);
    void* tree = root ? Spawn(P::name::WidgetTreeClass, root) : nullptr;
    void* ovl  = tree ? Spawn(L"Overlay", tree) : nullptr;
    if (!root || !tree || !ovl) return false;
    // The two back-pointers UMG writes for a cooked widget; without them the tree renders nothing
    // and reports no children.
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(root) + P::off::UUserWidget_WidgetTree) = tree;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(tree) + P::off::UWidgetTree_RootWidget) = ovl;

    // The scrim, as the game does it: a full-screen UImage with tint (0, 0, 0, 0.5) and no
    // ResourceObject, which dims the menu behind every native sub-screen; being Visible, it absorbs
    // a click that misses the window.
    void* scrim = Spawn(L"Image", ovl);
    if (!scrim) return false;
    U::SetImageTintRaw(scrim, FLinearColor{0.f, 0.f, 0.f, 0.5f});
    E::SetWidgetVisibility(scrim, 0);
    if (void* s = U::AddChild(ovl, scrim))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);

    // The window is a framed box: the game's own inst_uiBorder 9-slice around a #1A1A1A fill with
    // sharp corners (AddFramedBox).
    void* winBox = Spawn(L"SizeBox", ovl);
    void* winOvl = winBox ? AddFramedBox(winBox, Panel(), kBorderPx) : nullptr;
    void* col    = winOvl ? Spawn(L"VerticalBox", winOvl) : nullptr;
    if (!winBox || !winOvl || !col) return false;
    U::SetSizeBoxWidth(winBox, widthPx);
    U::SetSizeBoxHeight(winBox, heightPx);
    if (void* s = U::AddChild(winOvl, col)) {
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, kFill, kFill);
        SetSlotPadding(s, P::off::UOverlaySlot_Padding, kPadPx, kPadPx, kPadPx, kPadPx);
    }
    U::SetContent(winBox, winOvl);
    if (void* s = U::AddChild(ovl, winBox))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                        kCenter, kCenter);

    // The title strip: centred, white, larger, on a bordered strip of its own. White is reserved
    // for the title and body text; cyan appears in no VOTV menu.
    if (title) {
        if (void* titleBox = AddFramedBox(col, Panel(), kBorderPx)) {
            if (void* titleRow = Spawn(L"HorizontalBox", titleBox)) {
                AddText(titleRow, title, 24, Text(), kJustCenter, 1.f);
                if (void* s = U::AddChild(titleBox, titleRow))
                    U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                                    kFill, kCenter);
            }
            if (void* s = U::AddChild(col, titleBox))
                SetSlotPadding(s, P::off::UVerticalBoxSlot_Padding, 0.f, 0.f, 0.f, kPadPx);
        }
    }

    out.box    = winBox;
    out.root   = root;
    out.scrim  = scrim;
    out.column = col;
    return true;
}

void* g_borderDonor = nullptr;
bool  g_borderDonorTried = false;

void SetBorderDonor(void* donorImage) {
    // A donor whose brush carries no art is refused: AddFramedBox chooses its child order from
    // whether a donor exists, and FramedBoxParts tells the children apart by reading the border's
    // ResourceObject. A donor that resolved without art would build the framed order and be
    // classified flat, swapping every row's fill and border with no log line. Refusing collapses
    // the two predicates: a published donor always clones a brush with art (the clone zeroes only
    // the resource handle, never the object). On ui_saveSlots_C.button_back only three of four
    // brushes carry a ResourceObject, and a recook can move which.
    if (donorImage) {
        void* art = ReadPtr(donorImage, static_cast<int32_t>(P::off::UImage_Brush +
                                                             P::off::FSlateBrush_ResourceObject));
        if (!art) {
            UE_LOGW("native_screen: frame donor %p has no brush art (ResourceObject is null) -- "
                    "REFUSED; windows keep the flat border rather than a mis-ordered frame",
                    donorImage);
            g_borderDonorTried = true;
            return;
        }
    }
    g_borderDonor = donorImage;
    g_borderDonorTried = true;
}

void ForgetBorderDonor() {
    // On the menu-instance edge: the donor is a UImage owned by that menu's ui_settings, and kept
    // across a rebuild CloneStyle would copy 0x88 bytes out of a destroyed instance's widget.
    g_borderDonor = nullptr;
    g_borderDonorTried = false;
}

bool BorderDonorResolved() { return g_borderDonorTried; }

bool FramedBoxParts(void* overlay, FramedParts& out) {
    if (!overlay) return false;
    const int32_t n = U::ChildCount(overlay);
    if (n < 2) return false;
    void* c0 = U::ChildAt(overlay, 0);
    void* c1 = U::ChildAt(overlay, 1);
    if (!c0 || !c1) return false;
    // Both children must be UImages, or the read below interprets an arbitrary widget's bytes as a
    // brush pointer: in bounds for any UWidget, so no fault, only a confident wrong answer.
    // NameEquals is allocation-free; ClassNameOf would mint two wstrings per row per sync.
    if (!R::NameEquals(R::NameOf(R::ClassOf(c0)), P::name::ImageClass) ||
        !R::NameEquals(R::NameOf(R::ClassOf(c1)), P::name::ImageClass)) {
        return false;
    }
    out.content = n >= 3 ? U::ChildAt(overlay, 2) : nullptr;
    // Which image is the border is read, not counted: the framed and flat layouts are mirror images
    // (framed puts the fill under and the border on top; flat must do the opposite or a solid rect
    // covers the box), so the index cannot tell. A cloned frame carries inst_uiBorder in its
    // brush's ResourceObject; a tinted fill carries nothing there.
    const auto resourceObject = [](void* img) -> void* {
        return img ? *reinterpret_cast<void**>(static_cast<uint8_t*>(img) +
                                               P::off::UImage_Brush +
                                               P::off::FSlateBrush_ResourceObject)
                   : nullptr;
    };
    if (resourceObject(c1)) {           // framed: {face, edge, content}
        out.face = c0;
        out.edge = c1;
    } else {                            // flat: {edge, face, content}
        out.edge = c0;
        out.face = c1;
    }
    return true;
}

void* AddFramedBox(void* parent, const FLinearColor& fill, float borderPx) {
    void* box = Spawn(L"Overlay", parent);
    if (!box) return nullptr;
    void* edge = Spawn(L"Image", box);
    void* face = Spawn(L"Image", box);
    if (!edge || !face) return nullptr;
    // The frame is the game's own: image_border's brush is the material inst_uiBorder as a 9-slice
    // box, which is why the native frame has a different pair of greys on each edge (a bevel lit
    // top-left) and a single-colour rectangle read as foreign in any grey. The brush's
    // FSlateResourceHandle is unreflected -- a TSharedPtr a raw copy would alias with no AddRef --
    // so it goes through CloneStyle, which zeroes the handle and lets Slate rebuild it.
    bool framed = false;
    if (g_borderDonor) {
        static constexpr size_t kOneBrush[1] = {0};
        framed = U::CloneStyle(edge, P::off::UImage_Brush, g_borderDonor, P::off::UImage_Brush,
                               P::off::FSlateBrush_Size, kOneBrush, 1);
    }
    if (!framed) U::SetImageTintRaw(edge, Border());
    U::SetImageTintRaw(face, fill);
    // ESlateVisibility: Visible=0, Collapsed=1, Hidden=2, HitTestInvisible=3,
    // SelfHitTestInvisible=4. 3 draws and never eats a click; 2 does not draw at all.
    E::SetWidgetVisibility(edge, 3);
    E::SetWidgetVisibility(face, 3);
    // The child order differs by frame. Cloned: fill underneath at full size, border on top at full
    // size, no inset; an inset paints the fill over the brush's inner bands and clips the bevel to
    // its outermost step. One ring per box: a native window edge samples the grey pair once across
    // the title strip and twice across the list, so the second pair is the inner panel's own ring
    // flush against the window's, and more bevel comes from nesting boxes, never from doubling a
    // border. Flat (no donor): the edge under and the fill inset by the border width, since a flat
    // edge on top at full size would cover the box.
    if (framed) {
        if (void* s = U::AddChild(box, face))
            U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                            kFill, kFill);
        if (void* s = U::AddChild(box, edge))
            U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                            kFill, kFill);
    } else {
        if (void* s = U::AddChild(box, edge))
            U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                            kFill, kFill);
        if (void* s = U::AddChild(box, face)) {
            U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                            kFill, kFill);
            auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                                 P::off::UOverlaySlot_Padding);
            pad[0] = pad[1] = pad[2] = pad[3] = borderPx;
        }
    }
    return box;
}

// A chrome UButton with a text label, styled from a donor UButton. A real UButton, for the sound:
// the cloned style carries the game's press and hover FSlateSounds, and Slate's press visual
// comes free. The label is authored, never cloned (a donor UTextBlock reads null at some timings
// and the silent fallback is Roboto, centred, white). The label's padding is the hit area; an
// unpadded "X" is a target one glyph wide.
void* BuildButton(void* parent, void* donorBtn, const wchar_t* label, int32_t fontSize) {
    void* b = Spawn(P::name::ButtonClass, parent);
    if (!b) return nullptr;
    U::CloneButtonStyle(b, donorBtn);
    // A probe: the face, size and outline the game puts on the donor's own label, logged once per
    // donor for the first four, so "two faces or one" is a measurement rather than a screenshot
    // reading.
    static int sReported = 0;
    if (donorBtn && sReported < 4) {
        // A UButton is a UContentWidget: a panel with exactly one child.
        if (void* dt = U::ChildAt(donorBtn, 0)) {
            auto* d = reinterpret_cast<uint8_t*>(dt) + P::off::UTextBlock_Font;
            void* face = *reinterpret_cast<void**>(d);
            const int32_t sz = *reinterpret_cast<int32_t*>(d + P::off::FSlateFontInfo_Size);
            const int32_t ol = *reinterpret_cast<int32_t*>(
                d + P::off::FSlateFontInfo_OutlineSettings + P::off::FFontOutlineSettings_OutlineSize);
            UE_LOGW("native_screen[fontprobe] donor '%ls' label class=%ls font='%ls' size=%d "
                    "outline=%d -- ours will be font_ui size=%d outline=0",
                    R::ToString(R::NameOf(donorBtn)).c_str(), R::ClassNameOf(dt).c_str(),
                    face ? R::ToString(R::NameOf(face)).c_str() : L"<null>", sz, ol, fontSize);
            ++sReported;
        } else {
            UE_LOGW("native_screen[fontprobe] donor '%ls' has no content widget -- cannot read "
                    "the face the game gives its own buttons",
                    R::ToString(R::NameOf(donorBtn)).c_str());
            ++sReported;
        }
    }
    if (void* t = Spawn(P::name::TextBlockClass, b)) {
        // Orange, not white: every button label in VOTV is the accent orange (Hide all, Language,
        // Binds, Back, Reset all, Apply, the gamemode tabs), and white is the title and body text.
        // White on near-black is the maximum contrast the panel holds and reads heavy; our glyphs
        // carry less ink than the game's, so the weight was never the difference.
        U::StyleTextBlock(t, fontSize, Accent(), kJustCenter);
        E::SetWidgetText(t, label);
        U::SetContent(b, t);
        // SetContent created the UButtonSlot: centre the glyph and pad it out.
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

namespace {

// Two answers, hit or miss. A third, "this child starts under the cursor, so stop walking",
// rested on child order matching arranged top-to-bottom order, which a rebuilt or scrolled-out
// list does not honour, and one violation returned no row for the whole list.
bool Probe(void* panel, int32_t i, long cx, long cy,
           const ue_wrap::FVector2D& panelTl, const ue_wrap::FVector2D& panelSz) {
    void* child = U::ChildAt(panel, i);
    ue_wrap::FVector2D tl{}, sz{};
    if (!child || !U::WidgetScreenRect(child, tl, sz) || sz.X < 1.f || sz.Y < 1.f)
        return false;
    if (static_cast<long>(std::floor(tl.Y)) > cy) return false;
    // Clipped to the panel: a child scrolled out of view is not arranged, so its cached geometry is
    // whatever it was when it last was (rows have reported positions above the list's own top), and
    // a stale rect must not claim a cursor inside the viewport.
    const float top = tl.Y > panelTl.Y ? tl.Y : panelTl.Y;
    const float bot = (tl.Y + sz.Y) < (panelTl.Y + panelSz.Y) ? (tl.Y + sz.Y)
                                                              : (panelTl.Y + panelSz.Y);
    if (bot <= top) return false;   // entirely scrolled out
    // floor, not a truncating cast: static_cast rounds toward zero, so a negative coordinate would
    // round the other way and eat the left pixel column of every row.
    const bool in = cy >= static_cast<long>(std::floor(top)) &&
                    cy <  static_cast<long>(std::floor(bot)) &&
                    cx >= static_cast<long>(std::floor(tl.X)) &&
                    cx <  static_cast<long>(std::floor(tl.X + sz.X));
    return in;
}

}  // namespace

namespace {
int32_t g_activeIndex = -1;   // game thread only, rewritten at the top of every menu tick
}

void BeginMenuTick(void* switcher) {
    g_activeIndex = switcher ? ue_wrap::umg::SwitcherIndex(switcher) : -1;
}

int32_t ActiveIndex() { return g_activeIndex; }

int32_t SafePriorIndex(int32_t live, int32_t ourIndex, int32_t previous) {
    if (live < 0 || live == ourIndex) return previous;
    return live;
}

int32_t ChildAtCursor(void* panel, int32_t count, long cx, long cy, int32_t hint) {
    if (!panel || count <= 0) return -1;
    ue_wrap::FVector2D tl{}, sz{};
    if (!U::WidgetScreenRect(panel, tl, sz) || sz.X < 1.f || sz.Y < 1.f) return -1;
    if (cx < static_cast<long>(std::floor(tl.X)) ||
        cx >= static_cast<long>(std::floor(tl.X + sz.X)) ||
        cy < static_cast<long>(std::floor(tl.Y)) ||
        cy >= static_cast<long>(std::floor(tl.Y + sz.Y)))
        return -1;
    if (hint >= 0 && hint < count && Probe(panel, hint, cx, cy, tl, sz))
        return hint;
    for (int32_t i = 0; i < count; ++i) {
        if (i == hint) continue;   // already probed
        // No early break on a child below the cursor: a scrolled-out child's stale rect can read
        // far below, and the rows are rebuilt on every sync with no promise that child order
        // survives. One out-of-order child then ended the walk and returned -1, no row hovered and
        // none selectable, while the cursor sat inside a later child's rect. At most `count` rect
        // reads on a list bounded by kMaxRows, on a poll that runs only when the pointer or the
        // scroll moved.
        if (Probe(panel, i, cx, cy, tl, sz)) return i;
    }
    return -1;
}

bool CursorInWidgetSpace(long& outX, long& outY) {
    POINT c{};
    if (!::GetCursorPos(&c)) return false;
    // The client origin is half the transform; the other half is the viewport's UI scale, and
    // Slate's own inverse (CursorToWidgetAbsolute) is the only source that has both.
    POINT cli = c;
    if (HWND hwnd = ::GetActiveWindow()) ::ScreenToClient(hwnd, &cli);
    ue_wrap::FVector2D abs{};
    if (!U::CursorToWidgetAbsolute(
            ue_wrap::FVector2D{static_cast<float>(cli.x), static_cast<float>(cli.y)}, abs)) {
        // Fail closed, and say so once: a fallback to client pixels (the space measured wrong)
        // silently aimed every hit test at an offset on a scaled viewport instead of reporting that
        // it could not answer.
        static bool sSaidSo = false;
        if (!sSaidSo) {
            sSaidSo = true;
            UE_LOGE("native_screen[hit]: Slate's cursor transform would not resolve, so NO "
                    "hit test can be answered this session. Every hand-built row and field "
                    "will read not-hovered. This is a refusal, not a miss -- the previous "
                    "behaviour guessed in client pixels and was wrong at any UI scale.");
        }
        return false;
    }
    outX = static_cast<long>(abs.X);
    outY = static_cast<long>(abs.Y);
    return true;
}

// The one hit-test space: this once compared raw GetCursorPos (desktop pixels) against
// WidgetScreenRect (Slate absolute) while HoverTracker::Poll converted first, two hit tests in
// two spaces that agree only with the window unscaled at the origin; at any other scale the
// hosting window's rows and the text field's click-to-focus missed by the offset. Both go
// through CursorInWidgetSpace.
bool WidgetContains(void* w, long hx, long hy) {
    if (!w) return false;
    ue_wrap::FVector2D tl{}, sz{};
    if (!U::WidgetScreenRect(w, tl, sz) || sz.X < 1.f || sz.Y < 1.f) return false;
    return hx >= static_cast<long>(std::floor(tl.X)) &&
           hx <  static_cast<long>(std::floor(tl.X + sz.X)) &&
           hy >= static_cast<long>(std::floor(tl.Y)) &&
           hy <  static_cast<long>(std::floor(tl.Y + sz.Y));
}

bool CursorOverWidget(void* w) {
    if (!w) return false;
    long hx = 0, hy = 0;
    if (!CursorInWidgetSpace(hx, hy)) return false;
    return WidgetContains(w, hx, hy);
}

void HoverTracker::Reset() {
    lastX_ = lastY_ = -1;
    lastFrac_  = -2.f;
    lastCount_ = -1;
    index_     = -1;
    pending_   = false;
}

bool HoverTracker::Poll(void* panel, int32_t shownCount) {
    POINT c{};
    if (!::GetCursorPos(&c)) return false;
    const bool moved = (c.x != lastX_ || c.y != lastY_);
    lastX_ = c.x; lastY_ = c.y;

    // The pointer is not the only thing that moves a row under it: a wheel scroll moves the rows
    // with the cursor still, and a sync changes how many there are. On a failed read the fraction
    // keeps its old value rather than a sentinel, so an unreadable scroll degrades to cursor-only
    // rather than a permanent re-evaluation.
    float frac = lastFrac_;
    U::ViewOffsetFraction(panel, frac);
    const bool scrolled = (frac != lastFrac_) || (shownCount != lastCount_);
    lastFrac_  = frac;
    lastCount_ = shownCount;

    if (!moved && !scrolled && !pending_) return false;
    // One settling pass after motion stops: during a sweep the answer trails by a frame, and the
    // next tick would see no delta and never correct it.
    pending_ = moved || scrolled;

    // The cursor must be in the rect's space. GetCursorPos is desktop space and WidgetScreenRect is
    // Slate absolute; compared directly they agree only with the window at the desktop origin, so
    // the hit test worked in fullscreen and missed by the whole client origin in a window
    // (measured: a pointer on a row, and the desktop comparison looking 180 px further down the
    // list).
    POINT cli = c;
    if (HWND hwnd = ::GetActiveWindow()) ::ScreenToClient(hwnd, &cli);

    // Then Slate's own inverse, because the client origin was only half of it: the remaining term
    // is the viewport's UI scale, and CursorToWidgetAbsolute puts both sides of the comparison in
    // one space wherever the window sits. Through the shared converter, so this and
    // CursorOverWidget cannot drift into two spaces again; it fails closed.
    long hx = 0, hy = 0;
    if (!CursorInWidgetSpace(hx, hy)) { index_ = -1; return true; }

    // Always on for the first three hovers per process, at WARN so it flushes (INFO is buffered and
    // a killed process never writes it, which once left a field report with nothing to read).
    // VOTVCOOP_HIT_PROBE=N raises the cap for the lab, whose selftest spends the three before the
    // row phase begins.
    static const int sCap = [] {
        if (const char* v = std::getenv("VOTVCOOP_HIT_PROBE")) {
            const int n = std::atoi(v);
            if (n > 0) return n;
        }
        return 3;
    }();
    static int sTold = 0;
    if (moved && sTold < sCap) {
        ++sTold;
        ue_wrap::FVector2D ptl{}, psz{};
        const bool haveP = U::WidgetScreenRect(panel, ptl, psz);
        const int32_t hit = ChildAtCursor(panel, shownCount, hx, hy, -1);
        // The conversion is the only way this function gets a coordinate; a failure returned above.
        UE_LOGW("native_screen[hit] desktop=(%ld,%ld) slateAbs=(%ld,%ld) "
                "panel %s(%.0f,%.0f) %.0fx%.0f -> row=%d",
                c.x, c.y, hx, hy,
                haveP ? "" : "UNREAD ", ptl.X, ptl.Y, psz.X, psz.Y, hit);
        // A miss inside the panel is the interesting miss: when the cursor is inside the list and
        // no child claims it, the whole child table (index, rect, readable or not) goes to the log,
        // because the answer is a comparison across children that no single-row dump can carry.
        if (hit < 0 && haveP && sCap > 3 &&
            hx >= static_cast<long>(ptl.X) && hx < static_cast<long>(ptl.X + psz.X) &&
            hy >= static_cast<long>(ptl.Y) && hy < static_cast<long>(ptl.Y + psz.Y)) {
            UE_LOGW("native_screen[hit]   MISS INSIDE THE PANEL -- %d child(ren) follow",
                    shownCount);
            for (int32_t i = 0; i < shownCount && i < 64; ++i) {
                void* ch = U::ChildAt(panel, i);
                ue_wrap::FVector2D ctl{}, csz{};
                const bool haveC = ch && U::WidgetScreenRect(ch, ctl, csz);
                UE_LOGW("native_screen[hit]     child %2d %ls %s(%.0f,%.0f) %.0fx%.0f%s",
                        i, ch ? R::ClassNameOf(ch).c_str() : L"(null)",
                        haveC ? "" : "UNREAD ", ctl.X, ctl.Y, csz.X, csz.Y,
                        (haveC && hx >= static_cast<long>(ctl.X) &&
                         hx < static_cast<long>(ctl.X + csz.X) &&
                         hy >= static_cast<long>(ctl.Y) &&
                         hy < static_cast<long>(ctl.Y + csz.Y)) ? "  <== CONTAINS CURSOR" : "");
            }
        }
    }

    index_ = ChildAtCursor(panel, shownCount, hx, hy, index_);
    return true;
}

}  // namespace ui::native_screen
