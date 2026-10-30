// coop/dev/pause_menu_probe_bind.cpp -- the pause-menu probe's bound-delegate measurement (M5): whether a
// delegate bound to a native function can be caught by an interceptor on that function, and what that
// function costs the rest of the game. See coop/dev/pause_menu_probe.h.
//
// The probe fires a bound dynamic delegate the way the multicast broadcast loop does (each bound (object,
// function) pair through the detoured ProcessEvent), so the target need not be a Blueprint function and the
// interceptor, which keys on the UFunction, sees it. That proves the ProcessEvent route of a bound pair, not
// the engine's own broadcast.
// The target must be one the game never dispatches through ProcessEvent, or the interceptor would be asked
// about every other caller: a count of PE dispatches is what proves that, and a Blueprint calling a native
// function directly never passes ProcessEvent and so never reaches an interceptor either.

#include "pause_menu_probe_detail.h"

#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace coop::dev::pause_menu_probe::detail::bind {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace SG = ue_wrap::script_gate;
namespace F  = ue_wrap::facts;
using Clock = std::chrono::steady_clock;

UE_FACT_FN(kRowEnterPause, "pause_menu_probe", L"ui_menu_C", L"enterPause", Declared);
UE_FACT_FN(kRowResume, "pause_menu_probe", L"ui_menu_C", L"resume", Declared);
// The candidates: no-parameter natives of UWidget for a button, one-float setters of USlider for a slider.
// SetValue is not one: the rows' rebind uses it.
UE_FACT_FN(kRowForceLayout, "pause_menu_probe", L"Widget", L"ForceLayoutPrepass", Declared);
UE_FACT_FN(kRowInvalidate, "pause_menu_probe", L"Widget", L"InvalidateLayoutAndVolatility", Declared);
UE_FACT_FN(kRowResetCursor, "pause_menu_probe", L"Widget", L"ResetCursor", Declared);
UE_FACT_FN(kRowStepSize, "pause_menu_probe", L"Slider", L"SetStepSize", Declared);
UE_FACT_FN(kRowMinValue, "pause_menu_probe", L"Slider", L"SetMinValue", Declared);
UE_FACT_FN(kRowMaxValue, "pause_menu_probe", L"Slider", L"SetMaxValue", Declared);

// The steady-state window the candidates are counted over before one is bound.
constexpr auto kWindow = std::chrono::seconds(60);

// FScriptDelegate: a weak object pointer and the function's name. A multicast delegate is the array of them.
struct ScriptDelegate {
    int32_t objectIndex;
    int32_t objectSerial;
    R::FName functionName;
};
struct MulticastDelegate {
    ScriptDelegate* data;
    int32_t num;
    int32_t max;
};

struct Cand {
    const F::Function* row;
    bool slider;
    F::Resolved fn;
    bool usable = false;
    bool observed = false;
    uint64_t count = 0;     // dispatches not made by our own calls
    uint64_t coop = 0;      // dispatches made by our own calls
};
Cand g_c[] = {{&kRowForceLayout, false, {}},  {&kRowInvalidate, false, {}}, {&kRowResetCursor, false, {}},
              {&kRowStepSize, true, {}},      {&kRowMinValue, true, {}},    {&kRowMaxValue, true, {}}};

struct Seen {
    F::Resolved fn;
    bool peTaken = false;
    bool gateTaken = false;
    uint64_t pe = 0, peCoop = 0, gate = 0, gateOurs = 0;
};
Seen g_enter, g_resume;

Clock::time_point g_start{};
std::unique_ptr<SG::Hold> g_hold;
bool g_started = false;

// The chosen functions and the pool the interceptors recognise our widgets by.
F::Resolved g_btnFn, g_sldFn;
std::unordered_map<void*, int> g_btnRow, g_sldRow;
int g_hits = 0, g_lastRow = -1, g_passed = 0;
float g_lastFloat = 0.f;
int g_slotsLost = 0;

struct Saved {
    MulticastDelegate* md;
    MulticastDelegate orig;
};
std::vector<Saved> g_saved;
std::vector<ScriptDelegate> g_buf;  // reserved once, never grown: the delegates point into it

void OnCand(void* /*self*/, void* fn, void* /*params*/) {
    for (Cand& c : g_c)
        if (c.fn.Fn() == fn) { (R::InCoopDispatch() ? c.coop : c.count)++; return; }
}

void OnPe(void* /*self*/, void* fn, void* /*params*/) {
    Seen* s = fn == g_enter.fn.Fn() ? &g_enter : fn == g_resume.fn.Fn() ? &g_resume : nullptr;
    if (s) (R::InCoopDispatch() ? s->peCoop : s->pe)++;
}

void OnGate(const SG::Call& c) {
    Seen* s = c.function == g_enter.fn.Fn() ? &g_enter : c.function == g_resume.fn.Fn() ? &g_resume : nullptr;
    if (!s) return;
    ++s->gate;
    if (c.fromOurCode) ++s->gateOurs;
}

// Pre-dispatch, keyed on the UFunction: ours are swallowed (the body never runs), every other caller of the
// function passes through untouched and is counted.
bool OnButtonPress(void* self, void* /*params*/) {
    const auto it = g_btnRow.find(self);
    if (it == g_btnRow.end()) { ++g_passed; return false; }
    ++g_hits;
    g_lastRow = it->second;
    return true;
}

bool OnSliderMove(void* self, void* params) {
    const auto it = g_sldRow.find(self);
    if (it == g_sldRow.end()) { ++g_passed; return false; }
    ++g_hits;
    g_lastRow = it->second;
    if (params) std::memcpy(&g_lastFloat, params, sizeof(float));
    return true;
}

void LogSeen(const char* name, const Seen& s) {
    UE_LOGI("pmprobe m5 dispatch %s: pe=%llu (our calls %llu) gate=%llu (our calls %llu)", name,
            static_cast<unsigned long long>(s.pe), static_cast<unsigned long long>(s.peCoop),
            static_cast<unsigned long long>(s.gate), static_cast<unsigned long long>(s.gateOurs));
}

// A multicast delegate fired the way the broadcast loop does: each bound (object, function) pair dispatched
// through the engine's ProcessEvent entry, the one our detour sits on, with the delegate signature's parameter
// block.
// reflection::CallFunction calls that entry, not the detour's trampoline.
int Broadcast(void* widget, int32_t off, void* parms) {
    const auto* md = reinterpret_cast<const MulticastDelegate*>(static_cast<uint8_t*>(widget) + off);
    int fired = 0;
    for (int32_t i = 0; i < md->num && md->data; ++i) {
        const ScriptDelegate e = md->data[i];
        void* obj = R::ResolveWeakObject(e.objectIndex, e.objectSerial);
        if (!obj) continue;
        void* declaring = nullptr;
        void* fn = R::FindDispatchFunction(R::ClassOf(obj), R::ToString(e.functionName).c_str(), &declaring);
        if (!fn) continue;
        R::CallFunction(obj, fn, parms);
        ++fired;
    }
    return fired;
}

// Appends (widget, fnName) to the widget's delegate: into the engine's array when it has room, else into
// the buffer we own when it holds nothing. The original three words are kept to be written back.
bool BindOne(void* widget, int32_t off, const ScriptDelegate& entry) {
    auto* md = reinterpret_cast<MulticastDelegate*>(static_cast<uint8_t*>(widget) + off);
    g_saved.push_back({md, *md});
    if (md->num < md->max && md->data) {
        md->data[md->num++] = entry;
        return true;
    }
    if (md->max == 0 && md->num == 0 && g_buf.size() < g_buf.capacity()) {
        g_buf.push_back(entry);
        md->data = &g_buf.back();
        md->num = 1;
        md->max = 1;
        return true;
    }
    ++g_slotsLost;
    return false;
}

void Restore() {
    for (const Saved& s : g_saved) *s.md = s.orig;
    g_saved.clear();
    g_buf.clear();
}

// The delegate entry binding `widget` to the function named `fnName`.
ScriptDelegate EntryFor(void* widget, const R::FName& fnName) {
    const int32_t idx = R::InternalIndexOf(widget);
    return ScriptDelegate{idx, R::AllocateSlotSerial(idx), fnName};
}

// The interceptors and the pool, taken down; the delegates are written back first so that no engine array
// points into the buffer we own when the widgets can next be destroyed.
void Unbind() {
    Restore();
    if (g_btnFn) GT::UnregisterInterceptor(g_btnFn.Fn(), &OnButtonPress);
    if (g_sldFn) GT::UnregisterInterceptor(g_sldFn.Fn(), &OnSliderMove);
    g_btnFn = {};
    g_sldFn = {};
    g_btnRow.clear();
    g_sldRow.clear();
}

char g_why[320];

// The first usable candidate of the kind with no dispatch of the game's in the window; null when none. A
// candidate whose observer was not taken counted nothing, so it proves nothing and is not chosen.
Cand* Silent(bool slider) {
    for (Cand& c : g_c)
        if (c.slider == slider && c.usable && c.observed && c.count == 0) return &c;
    return nullptr;
}

void NoSilentWhy() {
    int n = std::snprintf(g_why, sizeof(g_why), "no silent candidate:");
    for (const Cand& c : g_c)
        if (c.usable && n > 0 && n < static_cast<int>(sizeof(g_why)))
            n += std::snprintf(g_why + n, sizeof(g_why) - static_cast<size_t>(n), " %ls=%llu(observed=%d)",
                               c.row->Name(), static_cast<unsigned long long>(c.count), c.observed ? 1 : 0);
}

// Presses one widget and judges it: exactly one interceptor hit, on the row it belongs to. `value` is the float
// a slider carries. Returns whether the press was right; logs the first rows' presses and any wrong one.
bool Press(const char* what, int row, int button, void* widget, int32_t off, bool slider, float value) {
    g_hits = 0;
    g_lastRow = -1;
    g_lastFloat = -1.f;
    alignas(8) uint8_t parms[16] = {};
    std::memcpy(parms, &value, sizeof(value));
    const int fired = Broadcast(widget, off, slider ? static_cast<void*>(parms) : nullptr);
    const bool ok = fired == 1 && g_hits == 1 && g_lastRow == row && (!slider || g_lastFloat == value);
    if (row < 2 || !ok) {
        char extra[64] = "";
        if (slider) std::snprintf(extra, sizeof(extra), " sent=%.3f carried=%.3f", value, g_lastFloat);
        UE_LOGI("pmprobe m5 press %s row=%d index=%d fired=%d interceptor_hits=%d resolved_row=%d%s -> %s", what, row,
                button, fired, g_hits, g_lastRow, extra, ok ? "ok" : "WRONG");
    }
    return ok;
}

}  // namespace

bool Start(const char** why) {
    if (g_started) return true;
    g_hold = std::make_unique<SG::Hold>("pmprobe");
    g_started = true;  // from here Release takes down whatever the rest of Start got registered
    g_start = Clock::now();
    g_enter.fn = F::Resolve(kRowEnterPause);
    g_resume.fn = F::Resolve(kRowResume);
    if (!g_enter.fn || !g_resume.fn) {
        *why = "enterPause or resume did not resolve as a row";
        return false;
    }
    for (Seen* s : {&g_enter, &g_resume}) {
        s->peTaken = GT::RegisterPostObserver(s->fn, &OnPe);
        s->gateTaken = SG::Watch(s->fn, s == &g_enter ? 1 : 2, nullptr, &OnGate);
    }
    UE_LOGI("pmprobe m5 registered enterPause observer=%d gate=%d resume observer=%d gate=%d",
            g_enter.peTaken ? 1 : 0, g_enter.gateTaken ? 1 : 0, g_resume.peTaken ? 1 : 0, g_resume.gateTaken ? 1 : 0);
    for (Cand& c : g_c) {
        c.fn = F::Resolve(*c.row);
        const int32_t bytes = c.fn ? R::FunctionFrameSize(c.fn.Fn()) : -1;
        c.usable = c.fn && bytes == (c.slider ? static_cast<int32_t>(sizeof(float)) : 0);
        if (c.usable) c.observed = GT::RegisterPostObserver(c.fn, &OnCand);
        UE_LOGI("pmprobe m5 candidate %ls.%ls resolved=%d frame_bytes=%d usable=%d observer=%d", c.row->Class(),
                c.row->Name(), c.fn ? 1 : 0, bytes, c.usable ? 1 : 0, c.observed ? 1 : 0);
    }
    return true;
}

void ReportDispatch(int cycle, const char* via) {
    if (!g_started) return;
    UE_LOGI("pmprobe m5 dispatch cycle=%d via=%s", cycle, via);
    LogSeen("enterPause", g_enter);
    LogSeen("resume", g_resume);
    g_enter.pe = g_enter.peCoop = g_enter.gate = g_enter.gateOurs = 0;
    g_resume.pe = g_resume.peCoop = g_resume.gate = g_resume.gateOurs = 0;
}

Step Advance(const char** why) {
    if (!g_started) { *why = "the measurement was never started"; return Step::Failed; }
    const double secs = std::chrono::duration<double>(Clock::now() - g_start).count();
    if (secs < std::chrono::duration<double>(kWindow).count()) return Step::Waiting;
    for (const Cand& c : g_c)
        if (c.usable)
            UE_LOGI("pmprobe m5 count %ls.%ls = %llu PE dispatches in %.1fs (+%llu made by our own calls)",
                    c.row->Class(), c.row->Name(), static_cast<unsigned long long>(c.count), secs,
                    static_cast<unsigned long long>(c.coop));
    Cand* btn = Silent(false);
    Cand* sld = Silent(true);
    if (!btn || !sld) {
        NoSilentWhy();
        *why = g_why;
        return Step::Failed;
    }
    const int n = rows::Count();
    void* const btnCls = R::FindClass(L"Button");
    void* const sldCls = R::FindClass(L"Slider");
    const int32_t onClicked = btnCls ? R::FindPropertyOffset(btnCls, L"OnClicked") : -1;
    const int32_t onSlide = sldCls ? R::FindPropertyOffset(sldCls, L"OnValueChanged") : -1;
    UE_LOGI("pmprobe m5 bind button=%ls slider=%ls rows=%d OnClicked@0x%X OnValueChanged@0x%X", btn->row->Name(),
            sld->row->Name(), n, onClicked, onSlide);
    const R::FName btnName = ue_wrap::fname_utils::StringToFName(btn->row->Name());
    const R::FName sldName = ue_wrap::fname_utils::StringToFName(sld->row->Name());
    if (n <= 0 || onClicked < 0 || onSlide < 0 || btnName.ComparisonIndex == 0 || sldName.ComparisonIndex == 0) {
        *why = "no rows, a delegate property did not resolve, or a function name did not convert";
        return Step::Failed;
    }
    g_btnFn = btn->fn;
    g_sldFn = sld->fn;
    g_buf.clear();
    g_buf.reserve(static_cast<size_t>(n) * (kButtons + 1));
    for (int i = 0; i < n; ++i) {
        for (int k = 0; k < kButtons; ++k) g_btnRow[rows::ButtonAt(i, k)] = i;
        g_sldRow[rows::SliderAt(i)] = i;
    }
    const bool taken = GT::RegisterInterceptor(g_btnFn, &OnButtonPress) && GT::RegisterInterceptor(g_sldFn, &OnSliderMove);
    UE_LOGI("pmprobe m5 interceptors registered=%d", taken ? 1 : 0);
    if (!taken) { *why = "an interceptor was not taken"; return Step::Failed; }
    for (int i = 0; i < n; ++i) {
        for (int k = 0; k < kButtons; ++k) BindOne(rows::ButtonAt(i, k), onClicked, EntryFor(rows::ButtonAt(i, k), btnName));
        BindOne(rows::SliderAt(i), onSlide, EntryFor(rows::SliderAt(i), sldName));
    }
    if (g_slotsLost > 0) { *why = "a delegate had no room and no buffer"; return Step::Failed; }
    int wrong = 0, presses = 0;
    for (int i = 0; i < n; ++i) {
        for (int k = 0; k < kButtons; ++k, ++presses)
            if (!Press("button", i, k, rows::ButtonAt(i, k), onClicked, false, 0.f)) ++wrong;
        ++presses;
        if (!Press("slider", i, 0, rows::SliderAt(i), onSlide, true, static_cast<float>(i + 1) / 100.f)) ++wrong;
    }
    UE_LOGI("pmprobe m5 presses=%d exactly_one_hit=%d wrong=%d; calls by widgets that are not ours passed through=%d",
            presses, presses - wrong, wrong, g_passed);
    const size_t bound = g_saved.size();
    Unbind();
    UE_LOGI("pmprobe m5 restored: delegates written back=%zu", bound);
    if (wrong != 0) *why = "a press did not reach the interceptor exactly once";
    return wrong == 0 ? Step::Done : Step::Failed;
}

void Release() {
    Unbind();
    if (!g_started) return;
    const double total = std::chrono::duration<double>(Clock::now() - g_start).count();
    for (const Cand& c : g_c)
        if (c.observed)
            UE_LOGI("pmprobe m5 count_total %ls.%ls = %llu PE dispatches in %.1fs since registration (+%llu by our own calls)",
                    c.row->Class(), c.row->Name(), static_cast<unsigned long long>(c.count), total,
                    static_cast<unsigned long long>(c.coop));
    for (Cand& c : g_c)
        if (c.observed) GT::UnregisterObservers(c.fn.Fn(), &OnCand);
    for (Seen* s : {&g_enter, &g_resume}) {
        if (s->peTaken) GT::UnregisterObservers(s->fn.Fn(), &OnPe);
        if (s->gateTaken) SG::Unwatch(s->fn, s == &g_enter ? 1 : 2, nullptr, &OnGate);
    }
    g_hold.reset();
    g_started = false;
}

}  // namespace coop::dev::pause_menu_probe::detail::bind
