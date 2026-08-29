// ui/native_screen.cpp -- see ui/native_screen.h for WHY.
//
// Every body here MOVED VERBATIM out of server_browser_native.cpp (2026-08-29). The
// comments came with them: they record measurements, and a measurement's comment is worth
// more where the code is than where the code used to be.

#include "ui/native_screen.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <cmath>

namespace ui::native_screen {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace U = ue_wrap::umg;
namespace P = ue_wrap::profile;

}  // namespace

// THE sRGB CONVERSION IS NOT OPTIONAL. Those are sRGB byte values; FLinearColor is LINEAR,
// and the framebuffer converts back on the way out. Writing 0x31/255 = 0.192 as a linear
// tint puts sRGB 0.48 on screen -- #7B, more than double the intended #31 -- so the whole
// palette would render washed out and no amount of re-picking values would fix it. This is
// the same transform UE's FLinearColor::FromSRGBColor applies.
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

void* ReadPtr(void* base, int32_t off) {
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
    U::SetImageTintRaw(edge, Border());
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
        U::StyleTextBlock(t, fontSize, Text(), kJustCenter);
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

}  // namespace ui::native_screen
