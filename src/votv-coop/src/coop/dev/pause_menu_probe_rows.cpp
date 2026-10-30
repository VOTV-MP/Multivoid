// coop/dev/pause_menu_probe_rows.cpp -- the pause-menu probe's rows measurement (M3): what a list of rows built
// the server browser's way costs inside the open menu, and what the game's own settings row class is. See
// coop/dev/pause_menu_probe.h.

#include "pause_menu_probe_detail.h"

#include "ui/native_screen.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace coop::dev::pause_menu_probe::detail::rows {
namespace {

namespace R  = ue_wrap::reflection;
namespace P  = ue_wrap::profile;
namespace E  = ue_wrap::engine;
namespace U  = ue_wrap::umg;
namespace NS = ui::native_screen;

constexpr int kSmall = 12;
constexpr int kLarge = 100;
constexpr int kRebinds = 60;
constexpr float kBoxW = 600.f, kBoxH = 320.f;

// One row: the server browser's cells in kind (a text, a slider, four buttons), the three texts a rebind
// writes and the slider value it sets.
struct Row {
    void* text = nullptr;
    void* slider = nullptr;
    void* btn[kButtons]{};
    void* lbl[2]{};
};

enum class St : uint8_t { Build12, Rebind12, SettingsMake, SettingsRead, Build100, Rebind100, Done };

St g_st = St::Build12;
void* g_menu = nullptr;
void* g_screen = nullptr;
void* g_scroll = nullptr;
void* g_donor = nullptr;
void* g_settings = nullptr;
std::vector<Row> g_rows;
int g_pass = 0;
double g_sumMs = 0.0, g_maxMs = 0.0;

double NowMs() {
    static const double perMs = [] { LARGE_INTEGER f{}; ::QueryPerformanceFrequency(&f); return f.QuadPart / 1000.0; }();
    LARGE_INTEGER t{};
    ::QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / perMs;
}

bool CanvasSlotVec(void* slot, const wchar_t* fn, const wchar_t* param, float x, float y) {
    void* f = R::FindFunction(R::ClassOf(slot), fn);
    if (!f) return false;
    ue_wrap::ParamFrame frame(f);
    return frame.valid() && frame.Set<ue_wrap::FVector2D>(param, ue_wrap::FVector2D{x, y}) &&
           ue_wrap::Call(slot, frame);
}

bool SetSliderValue(void* slider, float v) {
    static void* fn = nullptr;
    if (!fn) {
        void* c = R::FindClass(L"Slider");
        fn = c ? R::FindFunction(c, L"SetValue") : nullptr;
    }
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<float>(L"InValue", v) && ue_wrap::Call(slider, f);
}

bool ClearScroll() {
    static void* fn = nullptr;
    if (!fn) {
        void* c = R::FindClass(L"PanelWidget");
        fn = c ? R::FindFunction(c, L"ClearChildren") : nullptr;
    }
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(g_scroll, f);
}

bool BuildRow(Row& r) {
    void* hb = NS::Spawn(L"HorizontalBox", g_scroll);
    if (!hb || !U::AddChild(g_scroll, hb)) return false;
    r.text = NS::AddText(hb, L"row", 16, NS::Text(), NS::kJustLeft, 1.f);
    r.slider = NS::Spawn(L"Slider", hb);
    if (!r.text || !r.slider) return false;
    if (void* s = U::AddChild(hb, r.slider)) NS::SetHSlot(s, 1.f, NS::kFill, NS::kCenter);
    for (void*& b : r.btn) {
        b = NS::BuildButton(hb, g_donor, L"b", 14);
        if (!b) return false;
    }
    r.lbl[0] = U::ChildAt(r.btn[0], 0);
    r.lbl[1] = U::ChildAt(r.btn[1], 0);
    return r.lbl[0] && r.lbl[1];
}

bool Build(int n) {
    const double t0 = NowMs();
    g_rows.assign(static_cast<size_t>(n), Row{});
    for (Row& r : g_rows)
        if (!BuildRow(r)) return false;
    UE_LOGI("pmprobe m3 build rows=%d ms=%.2f children=%d", n, NowMs() - t0, U::ChildCount(g_scroll));
    g_pass = 0;
    g_sumMs = g_maxMs = 0.0;
    return true;
}

// One full rebind: three texts and the slider value of every row, as a list refreshed from new data would.
void Rebind() {
    const double t0 = NowMs();
    wchar_t buf[24];
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const Row& r = g_rows[i];
        std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"row %zu.%d", i, g_pass);
        E::SetWidgetText(r.text, buf);
        E::SetWidgetText(r.lbl[0], buf);
        E::SetWidgetText(r.lbl[1], buf);
        SetSliderValue(r.slider, static_cast<float>((i + static_cast<size_t>(g_pass)) % 100) / 100.f);
    }
    const double ms = NowMs() - t0;
    g_sumMs += ms;
    g_maxMs = (std::max)(g_maxMs, ms);
    ++g_pass;
}

void LogRebind() {
    UE_LOGI("pmprobe m3 rebind rows=%zu passes=%d mean=%.3f max=%.3f ms", g_rows.size(), g_pass,
            g_sumMs / g_pass, g_maxMs);
}

bool MakeSettingsRow(const char** why) {
    void* cls = R::FindClass(L"uicomp_settingsSlot_C");
    void* wbl = R::FindClassDefaultObject(L"WidgetBlueprintLibrary");
    void* fn = wbl ? R::FindFunction(R::ClassOf(wbl), L"Create") : nullptr;
    if (!cls || !fn) {
        UE_LOGI("pmprobe m3 settingsSlot SKIPPED: class=%p Create=%p", cls, fn);
        g_settings = nullptr;
        return true;
    }
    ue_wrap::ParamFrame f(fn);
    void* noPlayer = nullptr;
    if (!f.valid() || !f.Set(L"WorldContextObject", g_menu) || !f.Set(L"WidgetType", cls) ||
        !f.Set(L"OwningPlayer", noPlayer) || !ue_wrap::Call(wbl, f)) {
        *why = "WidgetBlueprintLibrary::Create failed for uicomp_settingsSlot_C";
        return false;
    }
    g_settings = f.Get<void*>(L"ReturnValue");
    if (!g_settings || !U::AddChild(g_scroll, g_settings)) {
        *why = "the settings row was not made or not attached";
        return false;
    }
    return true;
}

// The row class as the game builds it: attached, so its Construct has run a frame. Its Construct is
// beginCalc() in the game's blueprint dump: the name and button texts, the switcher's active index set to
// variableType, one text box's visibility; it writes no setting.
void ReadSettingsRow() {
    if (!g_settings) return;
    void* tree = NS::DonorField(g_settings, L"WidgetTree");
    void* root = NS::ReadPtr(tree, static_cast<int32_t>(P::off::UWidgetTree_RootWidget));
    DumpTree("m3 settingsSlot", root, 4, nullptr);
    const int32_t typeOff = R::FindPropertyOffset(R::ClassOf(g_settings), L"variableType");
    const int32_t type = typeOff >= 0 ? *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(g_settings) + typeOff) : -1;
    void* sw = NS::DonorField(g_settings, L"WidgetSwitcher_55");
    UE_LOGI("pmprobe m3 settingsSlot variableType=%d switcher_active=%d children=%d -> by default it shows the "
            "switcher child %d; its Construct (beginCalc) sets texts, that index and one visibility", type,
            sw ? U::SwitcherIndex(sw) : -2, sw ? U::ChildCount(sw) : -2, type);
}

}  // namespace

bool Begin(void* menu, void* screen, const char** why) {
    g_menu = menu;
    g_screen = screen;
    g_donor = NS::DonorField(menu, L"button_Resume");
    g_scroll = NS::Spawn(L"ScrollBox", screen);
    void* slot = g_scroll ? U::AddChild(screen, g_scroll) : nullptr;
    if (!g_donor || !slot) {
        *why = "the scroll box or the button donor could not be made";
        g_scroll = nullptr;
        return false;
    }
    CanvasSlotVec(slot, L"SetPosition", L"InPosition", 20.f, 10.f);
    CanvasSlotVec(slot, L"SetSize", L"InSize", kBoxW, kBoxH);
    g_st = St::Build12;
    return true;
}

Step Advance(const char** why) {
    if (!g_scroll || !R::IsLive(g_scroll)) { *why = "the scroll box was destroyed"; return Step::Failed; }
    switch (g_st) {
    case St::Build12:
    case St::Build100: {
        const int n = g_st == St::Build12 ? kSmall : kLarge;
        if (!Build(n)) { *why = "a row could not be built"; return Step::Failed; }
        g_st = g_st == St::Build12 ? St::Rebind12 : St::Rebind100;
        return Step::Running;
    }
    case St::Rebind12:
    case St::Rebind100:
        Rebind();
        if (g_pass < kRebinds) return Step::Running;
        LogRebind();
        if (g_st == St::Rebind100) {
            g_st = St::Done;
            return Step::Done;
        }
        if (!ClearScroll()) { *why = "the scroll box could not be cleared"; return Step::Failed; }
        g_rows.clear();
        g_st = St::SettingsMake;
        return Step::Running;
    case St::SettingsMake:
        if (!MakeSettingsRow(why)) return Step::Failed;
        g_st = St::SettingsRead;
        return Step::Running;
    case St::SettingsRead:
        ReadSettingsRow();
        g_settings = nullptr;
        if (!ClearScroll()) { *why = "the scroll box could not be cleared of the settings row"; return Step::Failed; }
        g_st = St::Build100;
        return Step::Running;
    case St::Done:
        return Step::Done;
    }
    return Step::Done;
}

void* Scroll() { return g_scroll; }

int Count() { return static_cast<int>(g_rows.size()); }

void* ButtonAt(int row, int button) {
    return row >= 0 && row < Count() && button >= 0 && button < kButtons ? g_rows[static_cast<size_t>(row)].btn[button]
                                                                         : nullptr;
}

void* SliderAt(int row) { return row >= 0 && row < Count() ? g_rows[static_cast<size_t>(row)].slider : nullptr; }

void Teardown() {
    if (g_scroll && g_screen && R::IsLive(g_screen)) U::RemoveChild(g_screen, g_scroll);
    g_scroll = nullptr;
    g_settings = nullptr;
    g_rows.clear();
}

}  // namespace coop::dev::pause_menu_probe::detail::rows
