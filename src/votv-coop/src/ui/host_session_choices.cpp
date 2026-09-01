// ui/host_session_choices.cpp -- see ui/host_session_choices.h.
//
// Extracted 2026-09-01 from host_session_settings.cpp, where the row kit and TWO
// hand-copied selectors lived. The construction and both style channels are MOVED
// verbatim; the only new code is the `Selector` indirection that lets one body serve
// three questions instead of three bodies serving one each.

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

// The row geometry the two shipped selectors were built with. Moved, not re-tuned: a
// constant re-picked during an extraction is a behaviour change wearing a refactor's
// clothes, and the screen's fixed-height frame is measured against exactly these.
constexpr float kRowH = 56.f;
// The label/detail split. Both shipped selectors passed 0.42/0.58 and neither had a
// reason to differ, so it is one constant here rather than a parameter nobody varies.
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

// MOVED VERBATIM from host_session_settings.cpp. The one edit is that the failure paths
// return an empty row rather than a `Row{}` typedef that no longer exists.
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
        // CHECKED BEFORE THE WIDGETS ARE STORED. The shipped WHO-MAY-JOIN loop stored
        // first and checked after, which is harmless there and was NOT harmless in the
        // SERVER LIST loop: a bare `return false` past `TF::Create` leaked a text field on
        // every retry, once a second forever after the backoff (post-ship audit
        // 2026-09-01). Storing only complete rows means a caller's failure path never has
        // to reason about which half of a selector exists.
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
        // TWO INDEPENDENT CHANNELS, style doc section 4: selection is the row FILL, hover
        // is the TEXT colour. A selector the player cannot move keeps its fill -- it is
        // still stating what will happen -- and loses the white, so the section reads as
        // information rather than as a control that ignores clicks.
        U::SetImageTint(s.bg[i], s.chosen == i ? kRowSel : kRowBg);
        const FLinearColor& c = !s.editable ? kDim : (s.hover == i ? kHover : kText);
        E::SetTextBlockColorDispatch(s.title[i], c);
    }
}

int HoverAt(const Selector& s, long hx, long hy) {
    if (!s.editable) return -1;
    // GEOMETRY, NEVER `IsHovered()`. A hand-built `UImage` set Visible reports 0 from
    // IsHovered whether or not it sits in a scroll container -- measured twice, on
    // 2026-08-29 and again 2026-08-30. That fact cost a shipped selector that never
    // highlighted; it is written here so the third question does not re-derive it.
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
