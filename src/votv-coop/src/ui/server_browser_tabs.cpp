// ui/server_browser_tabs.cpp -- see ui/server_browser_tabs.h.

#include "ui/server_browser_tabs.h"

#include "coop/net/master_slots.h"
#include "coop/text/utf8_codec.h"   // the labels are ASCII, but every widget text goes through one codec
#include "ui/native_screen.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <string>
#include <vector>

namespace ui::server_browser_tabs {
namespace {

namespace E  = ue_wrap::engine;
namespace U  = ue_wrap::umg;
namespace P  = ue_wrap::profile;
namespace NS = ui::native_screen;
namespace slots = coop::net::master_slots;

// A tab is a list row's box in a row of its own: the same frame, a shorter height (a label, not a
// server), and the kit's list gap between neighbours.
constexpr float kTabH        = 38.f;
constexpr float kTabBorderPx = 2.f;
constexpr int   kLabelPx     = 18;
// The label keeps clear of the frame: the ring's rendered width and a margin.
constexpr float kLabelInsetPx = NS::kNativeRingPx + 4.f;
// EStretch::ScaleToFit and EStretchDirection::DownOnly: six tabs share the list's width, and a
// long label is shrunk whole to fit its tab rather than cut.
constexpr uint8_t kScaleToFit = 2, kDownOnly = 1;

// What a tab was last drawn as, so a pointer move redraws the tabs it changed, not the strip.
constexpr int kLookSelected = 1, kLookLit = 2, kLookNever = -1;

struct TabParts {
    void* box  = nullptr;   // the hit target: the whole tab
    void* edge = nullptr;   // the frame, lit by the pointer
    void* face = nullptr;   // the fill, the selection
    void* text = nullptr;   // the label, lit by the pointer
    int   look = kLookNever;
};
void* g_strip = nullptr;        // the tabs' row: one rect read rules the pointer out of every tab
std::vector<TabParts> g_tabs;   // built once per menu instance
int  g_hover = -1;
long g_lastX = -1, g_lastY = -1;
bool g_settlePending = false;

// Draws each tab whose look changed; `all` draws every tab (a screen just shown).
void Paint(bool all) {
    const int sel = slots::SelectedIndex();
    for (int i = 0; i < static_cast<int>(g_tabs.size()); ++i) {
        TabParts& t = g_tabs[static_cast<size_t>(i)];
        const bool selected = (i == sel);
        const bool hovered = (i == g_hover);
        const bool lit = NS::PointerLit(hovered, selected);
        const int look = (selected ? kLookSelected : 0) | (lit ? kLookLit : 0);
        if (!all && look == t.look) continue;
        NS::ApplySelectableSkin(t.face, t.edge, hovered, selected);
        if (t.text) E::SetTextBlockColorDispatch(t.text, lit ? NS::Hover() : NS::Accent());
        t.look = look;
    }
}

// The tab under a pointer already in widget space, or -1. The strip first: most pointer moves on
// this screen are over the list, and there the answer is one rect read instead of one per tab.
int TabAt(long hx, long hy) {
    if (!NS::WidgetContains(g_strip, hx, hy)) return -1;
    for (int i = 0; i < static_cast<int>(g_tabs.size()); ++i)
        if (NS::WidgetContains(g_tabs[static_cast<size_t>(i)].box, hx, hy)) return i;
    return -1;
}

}  // namespace

bool Build(void* parent) {
    Forget();
    if (!parent) return false;
    const std::vector<slots::Slot> list = slots::List();
    void* strip = NS::Spawn(L"HorizontalBox", parent);
    if (!strip) return false;
    if (void* s = NS::AddVFill(parent, strip, 0.f, NS::kFill, NS::kTop))
        NS::SetSlotPadding(s, P::off::UVerticalBoxSlot_Padding, 0.f, 0.f, 0.f, NS::kListGapPx);
    for (size_t i = 0; i < list.size(); ++i) {
        TabParts t;
        t.box = NS::Spawn(L"SizeBox", strip);
        if (!t.box) return false;
        U::SetSizeBoxHeight(t.box, kTabH);
        // The kit's box does not attach itself; a SizeBox takes its one child through SetContent.
        void* ovl = NS::AddFramedBox(t.box, NS::RowBg(), kTabBorderPx);
        void* fit = ovl ? NS::Spawn(L"ScaleBox", ovl) : nullptr;
        if (!ovl || !fit) return false;
        // Filled both ways, so the tab's width is the one bound on the label: centred vertically,
        // the box would be given only the height it asked for, and its scale would feed back into
        // it.
        if (void* s = U::AddChild(ovl, fit)) {
            U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                            NS::kFill, NS::kFill);
            NS::SetSlotPadding(s, P::off::UOverlaySlot_Padding, kLabelInsetPx, 0.f,
                               kLabelInsetPx, 0.f);
        }
        if (!U::SetScaleBoxFit(fit, kScaleToFit, kDownOnly)) return false;
        const std::string& label = list[i].label;
        const std::wstring wide = coop::text::FromUtf8Lossy(label.data(), label.size());
        // The fit box's one child, centred by its slot's defaults.
        t.text = NS::AddText(fit, wide.c_str(), kLabelPx, NS::Accent(), NS::kJustCenter, 0.f);
        U::SetContent(t.box, ovl);
        NS::FramedParts parts;
        if (!t.text || !NS::FramedBoxParts(ovl, parts)) return false;
        t.edge = parts.edge;
        t.face = parts.face;
        // Equal widths across the strip, the list's gap between neighbours and none after the last.
        if (void* s = NS::AddHFill(strip, t.box, 1.f, NS::kFill, NS::kFill))
            NS::SetSlotPadding(s, P::off::UHorizontalBoxSlot_Padding, 0.f, 0.f,
                               i + 1 < list.size() ? NS::kListGapPx : 0.f, 0.f);
        g_tabs.push_back(t);
    }
    g_strip = strip;
    Paint(true);
    UE_LOGI("server_browser_tabs: %zu master tab(s) built, %s chosen", g_tabs.size(),
            slots::Selected().label.c_str());
    return true;
}

void Forget() {
    g_strip = nullptr;
    g_tabs.clear();
    g_hover = -1;
    g_lastX = g_lastY = -1;
    g_settlePending = false;
}

void Sync() {
    if (!g_tabs.empty()) Paint(true);
}

void UpdateHover() {
    if (g_tabs.empty()) return;
    POINT p{};
    if (!::GetCursorPos(&p)) return;
    const bool moved = (p.x != g_lastX || p.y != g_lastY);
    if (!moved && !g_settlePending) return;
    g_lastX = p.x;
    g_lastY = p.y;
    g_settlePending = moved;   // one more pass after motion stops: Slate lays out a tick behind
    long hx = 0, hy = 0;
    const int hit = NS::CursorInWidgetSpace(hx, hy) ? TabAt(hx, hy) : -1;
    if (hit == g_hover) return;
    g_hover = hit;
    Paint(false);
}

bool OnReleaseEdge(bool& switched) {
    switched = false;
    if (g_tabs.empty()) return false;
    long hx = 0, hy = 0;
    if (!NS::CursorInWidgetSpace(hx, hy)) return false;
    const int hit = TabAt(hx, hy);
    if (hit < 0) return false;
    switched = (hit != slots::SelectedIndex());
    // Consumed either way: a click on the tab already chosen is still this strip's click.
    if (switched) slots::Select(hit);
    Paint(false);
    return true;
}

int Count() { return static_cast<int>(g_tabs.size()); }

void* Tab(int i) {
    return (i >= 0 && i < static_cast<int>(g_tabs.size())) ? g_tabs[static_cast<size_t>(i)].box
                                                            : nullptr;
}

}  // namespace ui::server_browser_tabs
