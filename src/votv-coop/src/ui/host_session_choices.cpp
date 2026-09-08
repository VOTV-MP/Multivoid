// ui/host_session_choices.cpp -- see ui/host_session_choices.h.
//
// One two-answer selector, built once and driven by the caller through a `Selector` handle, so
// the host screen's three questions share a body instead of each carrying a hand-copied one.

#include "ui/host_session_choices.h"

#include "ui/native_screen.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <cstdint>
#include <string>

namespace ui::host_session_choices {
namespace {

namespace E  = ue_wrap::engine;
namespace U  = ue_wrap::umg;
namespace P  = ue_wrap::profile;
namespace NS = ui::native_screen;

using ue_wrap::FLinearColor;

// The row geometry every selector on the screen is built with. The screen's fixed-height
// frame is measured against exactly this height, so changing it moves the frame.
constexpr float kRowH = 56.f;
// The label/detail split. No question has a reason to differ, so it is one constant here
// rather than a parameter nobody varies.
constexpr float kColA = 0.42f;
constexpr float kColB = 0.58f;

const FLinearColor kRowBg  = NS::RowBg();
const FLinearColor kRowSel = NS::RowSel();
const FLinearColor kText   = NS::Text();
const FLinearColor kHover  = NS::Hover();
const FLinearColor kDim    = NS::Dim();

void SetText(void* block, const wchar_t* t, const FLinearColor& col) {
    if (!block) return;
    E::SetWidgetText(block, t);
    E::SetTextBlockColorDispatch(block, col);
}

struct BuiltRow { void* box; void* bg; void* a; void* b; };

// A row is a fixed-height overlay: an Image behind, set Visible so it is the hit target,
// and a two-cell horizontal box in front. Any half-built row is returned empty, so a caller
// never receives a row whose background exists and whose label does not.
BuiltRow BuildRow(void* parent, float wA, float wB) {
    BuiltRow r{};
    r.box = NS::Spawn(L"SizeBox", parent);
    if (!r.box) return r;
    U::SetSizeBoxHeight(r.box, kRowH);
    void* ovl = NS::Spawn(L"Overlay", r.box);
    if (!ovl) return BuiltRow{};
    r.bg = NS::Spawn(L"Image", ovl);
    if (!r.bg) return BuiltRow{};
    U::SetImageTintRaw(r.bg, kRowBg);
    E::SetWidgetVisibility(r.bg, 0);   // Visible: it is the hit target
    if (void* s = U::AddChild(ovl, r.bg))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                        NS::kFill, NS::kFill);
    if (void* hb = NS::Spawn(L"HorizontalBox", ovl)) {
        r.a = NS::AddText(hb, L"", 18, kText, NS::kJustLeft, wA);
        r.b = NS::AddText(hb, L"", 15, kDim,  NS::kJustLeft, wB);
        if (void* s = U::AddChild(ovl, hb)) {
            U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                            NS::kFill, NS::kCenter);
            auto* pad = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) +
                                                 P::off::UOverlaySlot_Padding);
            pad[0] = 12.f; pad[1] = 0.f; pad[2] = 12.f; pad[3] = 0.f;
        }
    }
    U::SetContent(r.box, ovl);
    if (!r.bg || !r.a) return BuiltRow{};
    U::AddChild(parent, r.box);
    return r;
}

}  // namespace

bool Build(void* column, const wchar_t* heading, const Answer (&answers)[2], Selector& out) {
    if (!column) return false;
    if (heading) NS::AddText(column, heading, 16, NS::Accent(), NS::kJustLeft, 0.f);
    for (int i = 0; i < 2; ++i) {
        BuiltRow r = BuildRow(column, kColA, kColB);
        // CHECKED BEFORE THE WIDGETS ARE STORED, so a caller's failure path never has to reason
        // about which half of a selector exists. The reverse order -- store, then check -- is
        // harmless only while nothing below the check can allocate: in the server-list screen the
        // same shape put a bare `return false` past a text-field construction, and the field leaked
        // once per retry for as long as the backoff kept retrying.
        if (!r.bg || !r.a) return false;
        out.bg[i]    = r.bg;
        out.title[i] = r.a;
        SetText(r.a, answers[i].title,  kText);
        SetText(r.b, answers[i].detail, kDim);
    }
    return true;
}

void Repaint(const Selector& s) {
    for (int i = 0; i < 2; ++i) {
        if (!s.bg[i]) continue;
        // TWO INDEPENDENT CHANNELS, the native treatment recorded under State in
        // docs/votv-ui-style.md: selection is the row FILL and hover is the TEXT colour. A selector
        // the player cannot move keeps its fill -- it is still stating what will happen -- and
        // loses the white, so the section reads as information rather than as a control that
        // ignores clicks.
        U::SetImageTint(s.bg[i], s.chosen == i ? kRowSel : kRowBg);
        const FLinearColor& c = !s.editable ? kDim : (s.hover == i ? kHover : kText);
        E::SetTextBlockColorDispatch(s.title[i], c);
    }
}

int HoverAt(const Selector& s, long hx, long hy) {
    if (!s.editable) return -1;
    // GEOMETRY, NEVER `IsHovered()`. A hand-built `UImage` set Visible reports false from
    // IsHovered whether or not it sits in a scroll container, which is measured and written down
    // here because believing IsHovered costs a selector that never highlights.
    for (int i = 0; i < 2; ++i)
        if (s.bg[i] && NS::WidgetContains(s.bg[i], hx, hy)) return i;
    return -1;
}

bool HandleClick(Selector& s, long hx, long hy, int& outChosen) {
    if (!s.editable) return false;
    for (int i = 0; i < 2; ++i) {
        if (!s.bg[i] || !NS::WidgetContains(s.bg[i], hx, hy)) continue;
        outChosen = i;
        return true;   // consumed even when the value did not move
    }
    return false;
}

void ClearWidgets(Selector& s) {
    for (int i = 0; i < 2; ++i) { s.bg[i] = nullptr; s.title[i] = nullptr; }
    s.hover = -1;
}

}  // namespace ui::host_session_choices
