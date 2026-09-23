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

// A tab is a list row's box in a row of its own: the same frame width, a shorter height (a label,
// not a server), and the same gap between neighbours as between rows.
constexpr float kTabH        = 38.f;
constexpr float kTabBorderPx = 2.f;
constexpr float kTabGapPx    = 4.f;
constexpr int   kLabelPx     = 18;

struct TabParts {
    void* box  = nullptr;   // the hit target: the whole tab
    void* edge = nullptr;   // the frame, lit by the pointer
    void* face = nullptr;   // the fill, the selection
    void* text = nullptr;   // the label, lit by the pointer
};
std::vector<TabParts> g_tabs;   // built once per menu instance, read-only after
int  g_hover        = -1;
int  g_paintedSel   = -2;       // what the last paint drew; -2 = never painted
int  g_paintedHover = -2;
long g_lastX = -1, g_lastY = -1;
bool g_settlePending = false;

void Paint(bool force) {
    const int sel = slots::SelectedIndex();
    if (!force && sel == g_paintedSel && g_hover == g_paintedHover) return;
    for (int i = 0; i < static_cast<int>(g_tabs.size()); ++i) {
        const TabParts& t = g_tabs[static_cast<size_t>(i)];
        const bool selected = (i == sel);
        // The rows' precedence (server_browser_rows.cpp PointerLit): the chosen tab keeps its fill
        // and its frame; the pointer lights only the others.
        const bool lit = (i == g_hover) && !selected;
        // The dispatch setters, never raw writes: these widgets are attached to Slate.
        if (t.face) U::SetImageTint(t.face, selected ? NS::RowSel() : NS::RowBg());
        if (t.edge) U::SetImageTint(t.edge, lit ? NS::Hover() : NS::Border());
        if (t.text) E::SetTextBlockColorDispatch(t.text, lit ? NS::Hover() : NS::Accent());
    }
    g_paintedSel = sel;
    g_paintedHover = g_hover;
}

int TabAt(long hx, long hy) {
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
        NS::SetSlotPadding(s, P::off::UVerticalBoxSlot_Padding, 0.f, 0.f, 0.f, kTabGapPx);
    for (size_t i = 0; i < list.size(); ++i) {
        TabParts t;
        t.box = NS::Spawn(L"SizeBox", strip);
        if (!t.box) return false;
        U::SetSizeBoxHeight(t.box, kTabH);
        // The kit's box does not attach itself; a SizeBox takes its one child through SetContent.
        void* ovl = NS::AddFramedBox(t.box, NS::RowBg(), kTabBorderPx);
        void* hb  = ovl ? NS::Spawn(L"HorizontalBox", ovl) : nullptr;
        if (!ovl || !hb) return false;
        if (void* s = U::AddChild(ovl, hb))
            U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                            NS::kFill, NS::kCenter);
        const std::string& label = list[i].label;
        const std::wstring wide = coop::text::FromUtf8Lossy(label.data(), label.size());
        t.text = NS::AddText(hb, wide.c_str(), kLabelPx, NS::Accent(), NS::kJustCenter, 1.f);
        // A weighted text carries the kit's right gutter, which would pull a centred label left.
        if (t.text)
            NS::SetSlotPadding(NS::SlotOf(t.text), P::off::UHorizontalBoxSlot_Padding,
                               0.f, 0.f, 0.f, 0.f);
        U::SetContent(t.box, ovl);
        NS::FramedParts parts;
        if (!t.text || !NS::FramedBoxParts(ovl, parts)) return false;
        t.edge = parts.edge;
        t.face = parts.face;
        // Equal widths across the strip, the rows' gap between neighbours and none after the last.
        if (void* s = NS::AddHFill(strip, t.box, 1.f, NS::kFill, NS::kFill))
            NS::SetSlotPadding(s, P::off::UHorizontalBoxSlot_Padding, 0.f, 0.f,
                               i + 1 < list.size() ? kTabGapPx : 0.f, 0.f);
        g_tabs.push_back(t);
    }
    Paint(true);
    UE_LOGI("server_browser_tabs: %zu master tab(s) built, %s chosen", g_tabs.size(),
            slots::Selected().label.c_str());
    return true;
}

void Forget() {
    g_tabs.clear();
    g_hover = -1;
    g_paintedSel = g_paintedHover = -2;
    g_lastX = g_lastY = -1;
    g_settlePending = false;
}

void Sync(bool force) {
    if (!g_tabs.empty()) Paint(force);
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
    Paint(true);
    return true;
}

int Count() { return static_cast<int>(g_tabs.size()); }

void* Tab(int i) {
    return (i >= 0 && i < static_cast<int>(g_tabs.size())) ? g_tabs[static_cast<size_t>(i)].box
                                                            : nullptr;
}

}  // namespace ui::server_browser_tabs
