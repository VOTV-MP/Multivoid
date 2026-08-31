// ui/server_browser_inline_input.cpp -- see ui/server_browser_inline_input.h.

#include "ui/server_browser_inline_input.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/session/session_manager.h"

#include "ui/native_screen.h"
#include "ui/native_text_field.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <string>

namespace ui::server_browser_inline_input {
namespace {

namespace U  = ue_wrap::umg;
namespace P  = ue_wrap::profile;
namespace NS = ui::native_screen;
namespace TF = ui::native_text_field;
namespace sm = coop::session_manager;

TF::Field* g_name = nullptr;
TF::Field* g_addr = nullptr;
std::string g_lastAppliedName;
std::string g_addrCache;
bool g_nameWasFocused = false;
bool g_addrSubmitPending = false;

// A LABELLED FIELD CELL: the caption above, the field under it, both left-aligned, the
// pair filling its share of the strip.
//
// STACKED, NOT SIDE BY SIDE. A label beside a field spends horizontal room on the caption,
// and horizontal room is exactly what an address needs -- putting the caption on its own
// line costs 18 px of height once for the whole strip and gives both fields the full width
// of their half.
TF::Field* LabelledCell(void* strip, const wchar_t* label, const wchar_t* hint,
                        int32_t maxLen, float weight, bool lastCell) {
    void* cell = NS::Spawn(L"VerticalBox", strip);
    if (!cell) return nullptr;
    if (void* s = NS::AddHFill(strip, cell, weight, NS::kFill, NS::kTop))
        NS::SetSlotPadding(s, P::off::UHorizontalBoxSlot_Padding,
                           0.f, 0.f, lastCell ? 0.f : 12.f, 0.f);
    NS::AddText(cell, label, 16, NS::Accent(), NS::kJustLeft, 0.f);
    // WIDTH 0 = TAKE THE SLOT. The field's own SizeBox is left unconstrained and its slot
    // is Fill, so it is exactly as wide as its half of the strip -- the layout decides, not
    // a constant, and a narrower window narrows the field instead of clipping it.
    TF::Field* f = TF::Create(cell, hint, maxLen, 0.f);
    if (f) {
        if (void* box = TF::Widget(f)) {
            NS::SetVSlot(NS::SlotOf(box), 0.f, NS::kFill, NS::kTop);
            NS::SetSlotPadding(NS::SlotOf(box), P::off::UVerticalBoxSlot_Padding,
                               0.f, 2.f, 0.f, 0.f);
        }
    }
    return f;
}

}  // namespace

bool Armed() {
    static const bool s =
        coop::config::ResolveFlag(::coop::config_registry::rows::browser_inline_input);
    return s;
}

bool Build(void* parent, void* donorBtn) {
    (void)donorBtn;   // no buttons of its own: the action grid's cells act on these fields
    if (!Armed()) return true;
    if (!parent) return false;

    // A FULL-WIDTH STRIP ACROSS THE BOTTOM OF THE WINDOW, under both columns.
    //
    // THE FIRST VERSION PUT THESE IN THE RIGHT-HAND COLUMN, UNDER THE SERVER INFO, AND THE
    // USER CUT THAT ON SIGHT: "your name and connect by address don't belong on the right
    // panel with the server info - that takes too much space" (2026-08-31). They were
    // right about the room -- that column is ~330 px wide and already carries the details
    // panel, the Connect button and the status pane, so two fields squeezed a pane that
    // exists to be read. Across the window there is ~950 px, the two cells sit side by side
    // with their captions, and neither column loses anything but the strip's own height.
    //
    // This is also what separates the two variants HONESTLY rather than by placement luck:
    // A keeps the browser free of any input at all, B gives the input its own band and
    // never crowds the information.
    void* box = NS::AddFramedBox(parent, NS::Panel(), 2.f);
    void* strip = box ? NS::Spawn(L"HorizontalBox", box) : nullptr;
    if (!box || !strip) return false;
    if (void* s = U::AddChild(box, strip)) {
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                        NS::kFill, NS::kTop);
        NS::SetSlotPadding(s, P::off::UOverlaySlot_Padding, 10.f, 6.f, 10.f, 8.f);
    }
    if (void* s = NS::AddVFill(parent, box, 0.f, NS::kFill, NS::kTop))
        NS::SetSlotPadding(s, P::off::UVerticalBoxSlot_Padding, 0.f, 6.f, 0.f, 0.f);

    // The address gets the larger half: a name is a word and an address is host:port.
    g_name = LabelledCell(strip, L"Your name", L"your name", 24, 0.38f, false);
    g_addr = LabelledCell(strip, L"Connect by address", L"host or host:port", 64, 0.62f, true);
    if (!g_name || !g_addr) {
        UE_LOGE("server_browser_inline_input: could not build the inline fields "
                "(name=%p addr=%p)", static_cast<void*>(g_name), static_cast<void*>(g_addr));
        return false;
    }

    // PREFILLED FROM THE ROWS THEY WRITE. Both are values the player already has, and the
    // address field's whole reason to exist is the one they used last.
    g_lastAppliedName = sm::Nickname();
    TF::SetText(g_name, g_lastAppliedName);
    TF::SetText(g_addr,
                coop::config::ResolveString(::coop::config_registry::rows::browser_lastdirect));
    return true;
}

void Tick() {
    if (!g_name || !g_addr) return;
    TF::Tick(g_name);
    TF::Tick(g_addr);

    // THE NAME COMMITS WHEN THE PLAYER FINISHES WITH IT -- on Enter, or on the falling edge
    // of focus. There is no OK button for it, and that is the point of this variant: the
    // field IS the control. Writing on every keystroke would put a partial name on the wire
    // and an ini write per character; writing never would make the field a decoration.
    const bool focused = TF::Focused(g_name);
    const bool submitted = TF::ConsumeSubmit(g_name);
    if (submitted || (g_nameWasFocused && !focused)) {
        const std::string v = TF::Text(g_name);
        // EMPTY IS NOT A NAME, and it is not an error either -- the player cleared the box
        // and clicked away. Restore what they had rather than accepting nothing or
        // shouting at them.
        if (v.empty()) {
            TF::SetText(g_name, g_lastAppliedName);
        } else if (v != g_lastAppliedName) {
            g_lastAppliedName = v;
            sm::SetNickname(v);
            coop::config::WriteIniValue(::coop::config_registry::rows::net_nick, v.c_str());
            UE_LOGI("server_browser_inline_input: nickname committed from the inline field");
        }
    }
    g_nameWasFocused = focused;

    // The ADDRESS commits on Enter only -- it is an ACTION, not a setting, so leaving the
    // field must not dial anything. Enter routes through the same handler the grid's
    // "Direct connect" cell uses, which is why it is consumed here and answered there.
    if (TF::ConsumeSubmit(g_addr)) g_addrSubmitPending = true;
}

bool ConsumeAddressSubmit() {
    const bool v = g_addrSubmitPending;
    g_addrSubmitPending = false;
    return v;
}

bool OnReleaseEdge() {
    // The fields hit-test themselves by GEOMETRY inside their own Tick (click-to-focus),
    // which is the mechanism a hand-built UImage frame needs -- `IsHovered` does not answer
    // on one (native_screen.h). So there is nothing for the release edge to route here, and
    // saying so explicitly is better than an empty function nobody can explain.
    return false;
}

const char* Address() {
    if (!g_addr) return "";
    g_addrCache = TF::Text(g_addr);
    return g_addrCache.c_str();
}

void Forget() {
    TF::Destroy(g_name);
    TF::Destroy(g_addr);
    g_name = g_addr = nullptr;
    g_lastAppliedName.clear();
    g_nameWasFocused = false;
    g_addrSubmitPending = false;
}

}  // namespace ui::server_browser_inline_input
