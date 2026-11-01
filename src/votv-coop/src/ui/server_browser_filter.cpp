// ui/server_browser_filter.cpp -- see ui/server_browser_filter.h.

#include "ui/server_browser_filter.h"

#include "l10n/l10n.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/text/utf8_codec.h"
#include "ui/native_screen.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <atomic>
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
constexpr float kLeftGapPx  = 16.f;  // from Back
constexpr float kLabelGapPx = 8.f;
constexpr int   kLabelPx    = 16;

// -1 until the row is first read.
std::atomic<int> g_hide{-1};

void* g_row  = nullptr;   // the hit target: the box and its label
void* g_edge = nullptr;
void* g_face = nullptr;
void* g_tick = nullptr;
void* g_text = nullptr;
bool  g_hovered = false;
long  g_lastX = -1, g_lastY = -1;
bool  g_settlePending = false;

void Paint() {
    if (!g_row) return;
    NS::ApplySelectableSkin(g_face, g_edge, g_hovered, false);
    // ESlateVisibility: HitTestInvisible=3 draws and never eats a click, Hidden=2 keeps the box's shape.
    if (g_tick) E::SetWidgetVisibility(g_tick, HideIncompatible() ? 3 : 2);
    if (g_text) E::SetTextBlockColorDispatch(g_text, g_hovered ? NS::Hover() : NS::Text());
}

}  // namespace

bool HideIncompatible() {
    int v = g_hide.load(std::memory_order_relaxed);
    if (v < 0) {
        v = coop::config::ResolveFlag(coop::config_registry::rows::browser_hide_incompatible) ? 1 : 0;
        g_hide.store(v, std::memory_order_relaxed);
    }
    return v != 0;
}

void SetHideIncompatible(bool hide) {
    g_hide.store(hide ? 1 : 0, std::memory_order_relaxed);
    if (coop::config::SetValue(coop::config_registry::rows::browser_hide_incompatible, hide ? "1" : "0") ==
        coop::config::SetResult::Saved)
        UE_LOGI("server_browser_filter: incompatible servers %s (remembered)", hide ? "hidden" : "shown");
    else
        UE_LOGW("server_browser_filter: incompatible servers %s, but multivoid.ini could not remember it", hide ? "hidden" : "shown");
    Paint();
}

bool Build(void* parent) {
    Forget();
    if (!parent) return false;
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
    const std::wstring label = coop::text::FromUtf8Lossy(l10n::Tc("server_browser", "Hide incompatible servers"));
    void* text = NS::AddText(row, label.c_str(), kLabelPx, NS::Text(), NS::kJustLeft, 0.f);
    if (!text) return false;

    g_row = row;
    g_edge = parts.edge;
    g_face = parts.face;
    g_tick = tick;
    g_text = text;
    Paint();
    return true;
}

void Forget() {
    g_row = g_edge = g_face = g_tick = g_text = nullptr;
    g_hovered = false;
    g_lastX = g_lastY = -1;
    g_settlePending = false;
}

void Sync() { Paint(); }

void UpdateHover() {
    if (!g_row) return;
    POINT p{};
    if (!::GetCursorPos(&p)) return;
    const bool moved = (p.x != g_lastX || p.y != g_lastY);
    if (!moved && !g_settlePending) return;
    g_lastX = p.x;
    g_lastY = p.y;
    g_settlePending = moved;   // one more pass after motion stops: Slate lays out a tick behind
    long hx = 0, hy = 0;
    const bool hit = NS::CursorInWidgetSpace(hx, hy) && NS::WidgetContains(g_row, hx, hy);
    if (hit == g_hovered) return;
    g_hovered = hit;
    Paint();
}

bool OnReleaseEdge(bool& changed) {
    changed = false;
    if (!g_row) return false;
    long hx = 0, hy = 0;
    if (!NS::CursorInWidgetSpace(hx, hy) || !NS::WidgetContains(g_row, hx, hy)) return false;
    SetHideIncompatible(!HideIncompatible());
    changed = true;
    return true;
}

void* Box() { return g_row; }

}  // namespace ui::server_browser_filter
