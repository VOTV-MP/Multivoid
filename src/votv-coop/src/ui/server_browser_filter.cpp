// ui/server_browser_filter.cpp -- see ui/server_browser_filter.h.

#include "ui/server_browser_filter.h"

#include "l10n/l10n.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/lobby_client.h"
#include "coop/text/utf8_codec.h"
#include "ui/native_screen.h"
#include "ui/server_browser_rows.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <atomic>
#include <iterator>
#include <string>

namespace ui::server_browser_filter {
namespace {

namespace E  = ue_wrap::engine;
namespace U  = ue_wrap::umg;
namespace P  = ue_wrap::profile;
namespace NS = ui::native_screen;

constexpr float kBoxPx      = 26.f;
constexpr float kBoxBorder  = 2.f;
constexpr float kTickInset  = 7.f;   // the fill keeps clear of the frame
constexpr float kLeftGapPx  = 16.f;  // from Back, and from the box before
constexpr float kLabelGapPx = 8.f;
constexpr int   kLabelPx    = 16;   // with Back in the list's column: longer words ran under the status pane

struct Control {
    const coop::config_registry::FlagRow& row;
    const char* label;
    const char* what;   // for the log
    std::atomic<int> hide{-1};   // -1 until the row is first read
    void* box  = nullptr;   // the hit target: the box and its label
    void* edge = nullptr;
    void* face = nullptr;
    void* tick = nullptr;
    void* text = nullptr;
    bool  hovered = false;
};

Control g_controls[] = {
    {coop::config_registry::rows::browser_hide_incompatible, L10N_MARK_C("server_browser", "Hide incompatible"), "incompatible servers"},
    {coop::config_registry::rows::browser_hide_locked, L10N_MARK_C("server_browser", "Hide locked"), "locked servers"},
};

long g_lastX = -1, g_lastY = -1;
bool g_settlePending = false;

Control& Of(Filter f) { return g_controls[static_cast<int>(f)]; }

bool Read(Control& c) {
    int v = c.hide.load(std::memory_order_relaxed);
    if (v < 0) {
        v = coop::config::ResolveFlag(c.row) ? 1 : 0;
        c.hide.store(v, std::memory_order_relaxed);
    }
    return v != 0;
}

void Paint(Control& c) {
    if (!c.box) return;
    NS::ApplySelectableSkin(c.face, c.edge, c.hovered, false);
    // ESlateVisibility: HitTestInvisible=3 draws and never eats a click, Hidden=2 keeps the box's shape.
    if (c.tick) E::SetWidgetVisibility(c.tick, Read(c) ? 3 : 2);
    if (c.text) E::SetTextBlockColorDispatch(c.text, c.hovered ? NS::Hover() : NS::Text());
}

bool BuildOne(void* parent, Control& c) {
    void* row = NS::Spawn(L"HorizontalBox", parent);
    if (!row) return false;
    if (void* s = NS::AddHFill(parent, row, 0.f, NS::kLeft, NS::kCenter))
        NS::SetSlotPadding(s, P::off::UHorizontalBoxSlot_Padding, kLeftGapPx, 0.f, 0.f, 0.f);

    void* box = NS::Spawn(L"SizeBox", row);
    void* ovl = box ? NS::AddFramedBox(box, NS::RowBg(), kBoxBorder) : nullptr;
    void* tick = ovl ? NS::Spawn(L"Image", ovl) : nullptr;
    if (!box || !ovl || !tick) return false;
    U::SetSizeBoxWidth(box, kBoxPx);
    U::SetSizeBoxHeight(box, kBoxPx);
    U::SetImageTintRaw(tick, NS::Accent());
    if (void* s = U::AddChild(ovl, tick)) {
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, NS::kFill, NS::kFill);
        NS::SetSlotPadding(s, P::off::UOverlaySlot_Padding, kTickInset, kTickInset, kTickInset, kTickInset);
    }
    U::SetContent(box, ovl);
    if (void* s = NS::AddHFill(row, box, 0.f, NS::kLeft, NS::kCenter))
        NS::SetSlotPadding(s, P::off::UHorizontalBoxSlot_Padding, 0.f, 0.f, kLabelGapPx, 0.f);

    NS::FramedParts parts;
    if (!NS::FramedBoxParts(ovl, parts)) return false;
    const std::wstring label = coop::text::FromUtf8Lossy(l10n::Tc("server_browser", c.label));
    void* text = NS::AddText(row, label.c_str(), kLabelPx, NS::Text(), NS::kJustLeft, 0.f);
    if (!text) return false;

    c.box = row;
    c.edge = parts.edge;
    c.face = parts.face;
    c.tick = tick;
    c.text = text;
    Paint(c);
    return true;
}

}  // namespace

bool Hides(Filter f) { return Read(Of(f)); }

void SetHides(Filter f, bool hide) {
    Control& c = Of(f);
    c.hide.store(hide ? 1 : 0, std::memory_order_relaxed);
    if (coop::config::SetValue(c.row, hide ? "1" : "0") == coop::config::SetResult::Saved)
        UE_LOGI("server_browser_filter: %s %s (remembered)", c.what, hide ? "hidden" : "shown");
    else
        UE_LOGW("server_browser_filter: %s %s, but multivoid.ini could not remember it", c.what, hide ? "hidden" : "shown");
    Paint(c);
}

bool Leaves(const coop::net::lobby::LobbyRow& r) {
    return (r.locked && Hides(Filter::Locked)) ||
           (Hides(Filter::Incompatible) && ui::server_browser_rows::IsIncompatible(r));
}

bool Build(void* parent) {
    Forget();
    if (!parent) return false;
    for (Control& c : g_controls)
        if (!BuildOne(parent, c)) return false;
    return true;
}

void Forget() {
    for (Control& c : g_controls) {
        c.box = c.edge = c.face = c.tick = c.text = nullptr;
        c.hovered = false;
    }
    g_lastX = g_lastY = -1;
    g_settlePending = false;
}

void Sync() {
    for (Control& c : g_controls) Paint(c);
}

void UpdateHover() {
    if (!g_controls[0].box) return;
    POINT p{};
    if (!::GetCursorPos(&p)) return;
    const bool moved = (p.x != g_lastX || p.y != g_lastY);
    if (!moved && !g_settlePending) return;
    g_lastX = p.x;
    g_lastY = p.y;
    g_settlePending = moved;   // one more pass after motion stops: Slate lays out a tick behind
    long hx = 0, hy = 0;
    const bool placed = NS::CursorInWidgetSpace(hx, hy);
    for (Control& c : g_controls) {
        const bool hit = placed && c.box && NS::WidgetContains(c.box, hx, hy);
        if (hit == c.hovered) continue;
        c.hovered = hit;
        Paint(c);
    }
}

bool OnReleaseEdge(bool& changed) {
    changed = false;
    long hx = 0, hy = 0;
    if (!g_controls[0].box || !NS::CursorInWidgetSpace(hx, hy)) return false;
    for (int i = 0; i < static_cast<int>(std::size(g_controls)); ++i) {
        if (!g_controls[i].box || !NS::WidgetContains(g_controls[i].box, hx, hy)) continue;
        const Filter f = static_cast<Filter>(i);
        SetHides(f, !Hides(f));
        changed = true;
        return true;
    }
    return false;
}

void* Box(Filter f) { return Of(f).box; }

}  // namespace ui::server_browser_filter
