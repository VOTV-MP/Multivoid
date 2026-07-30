// coop/dev/input_focus_probe.cpp -- DIAGNOSTIC (VOTVCOOP_INPUT_PROBE=1).
//
// Answers the load-bearing unknowns of the input-ownership arc (design fact base
// research/findings/tooling/votv-input-ownership-FACTS-2026-07-31.md):
//
//   M1  Does a reflected `UWidget::HasKeyboardFocus()` actually report Slate keyboard
//       focus on VOTV's editable widgets? The whole arc hangs on this: the game's 73
//       text fields are 3 engine widget CLASSES (UEditableTextBox x66,
//       UMultiLineEditableText x5, UMultiLineEditableTextBox x2), so a per-class scan
//       is an invariant where a per-surface allowlist would be a 26-row site list.
//       Its RED is built in: the same line reports the count of live editable widgets
//       and how many have focus, so "nothing focused" must read focus=0 with count>0.
//       A predicate that is always false is therefore distinguishable from a working
//       one -- an always-false read would show count>0 focus=0 in EVERY sample
//       including the ones taken while a field is demonstrably focused.
//   M1b The cheaper secondary: `mainPlayer.activeInterface` (0x07E0) and, when it is
//       non-null, `HasFocusedDescendants()` on it. Measured to be BROADER than "a text
//       field is focused" (panel_radar is an interface with no text field), so the two
//       reads are logged side by side rather than one standing in for the other.
//   M5  Does the camera spin while one of our surfaces is up? VOTV calls SetCursorPos
//       ~120x/s and our SetCursorPosDetour no-ops all of them while capture is active.
//       If mouselook is poll-based (GetCursorPos minus centre) rather than raw-input,
//       suppressing the recentre feeds the same delta every tick forever. ControlRotation
//       yaw is logged so a spin is visible as a monotonic drift rather than inferred.
//
// Everything here is read-only and env-gated; nothing is installed when the env is
// absent. RULE 2 exempts probes/diagnostics (feedback_rule2_exempts_probes_diagnostics_tools).

#include "coop/dev/input_focus_probe.h"

#include <windows.h>

#include <cstdint>
#include <string>

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

namespace R = ue_wrap::reflection;

namespace coop::dev::input_focus_probe {
namespace {

bool g_armed = false;
bool g_checked = false;

// The three engine widget classes that hold every one of the game's 73 measured
// editable-text fields. Resolved once; a null entry just contributes nothing.
void* g_clsEditableTextBox = nullptr;
void* g_clsMultiLineText = nullptr;
void* g_clsMultiLineTextBox = nullptr;
void* g_fnHasKeyboardFocus = nullptr;
void* g_fnHasFocusedDescendants = nullptr;
void* g_fnSetKeyboardFocus = nullptr;
void* g_fnHasAnyUserFocus = nullptr;
void* g_fnHasUserFocusedDesc = nullptr;
bool  g_classesResolved = false;

// mainPlayer.activeInterface (0x07E0 by the dump; resolved by name, never hardcoded).
int32_t g_activeInterfaceOff = -2;
int32_t g_controlRotationOff = -2;

bool Armed() {
    if (!g_checked) {
        g_checked = true;
        char v[8]{};
        g_armed = ::GetEnvironmentVariableA("VOTVCOOP_INPUT_PROBE", v, sizeof(v)) > 0 && v[0] == '1';
    }
    return g_armed;
}

void ResolveOnce() {
    if (g_classesResolved) return;
    g_classesResolved = true;
    g_clsEditableTextBox = R::FindClass(L"EditableTextBox");
    g_clsMultiLineText = R::FindClass(L"MultiLineEditableText");
    g_clsMultiLineTextBox = R::FindClass(L"MultiLineEditableTextBox");
    void* widgetCls = R::FindClass(L"Widget");
    if (widgetCls) {
        g_fnHasKeyboardFocus = R::FindFunction(widgetCls, L"HasKeyboardFocus");
        g_fnHasFocusedDescendants = R::FindFunction(widgetCls, L"HasFocusedDescendants");
        g_fnSetKeyboardFocus = R::FindFunction(widgetCls, L"SetKeyboardFocus");
        g_fnHasAnyUserFocus = R::FindFunction(widgetCls, L"HasAnyUserFocus");
        g_fnHasUserFocusedDesc = R::FindFunction(widgetCls, L"HasUserFocusedDescendants");
    }
    UE_LOGI("input_probe: resolve EditableTextBox=%p MultiLineEditableText=%p "
            "MultiLineEditableTextBox=%p Widget=%p HasKeyboardFocus=%p HasFocusedDescendants=%p",
            g_clsEditableTextBox, g_clsMultiLineText, g_clsMultiLineTextBox, widgetCls,
            g_fnHasKeyboardFocus, g_fnHasFocusedDescendants);
}

// The logger's vsnprintf drops the whole line on a %ls it cannot encode (measured
// 2026-07-28, the arc-D2 gate run read as a relay failure because of it). Widget names
// are ASCII, but a probe whose ABSENCE looks like a negative result is exactly the
// instrument trap this project has paid for twice -- so narrow every name first.
std::string Narrow(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (wchar_t c : w) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

// A live widget INSTANCE, as opposed to the two things that share its class and can
// never have Slate focus: the class default object (name "Default__*") and the widget
// TEMPLATE stored inside a WidgetBlueprintGeneratedClass (its Outer IS that class).
// Run 1 of this probe asked the CDO and reported "PREDICATE DEAD"; that was the
// instrument, and it is exactly why the round trip prints what it targeted.
bool IsLiveInstance(void* o) {
    if (R::ToString(R::NameOf(o)).rfind(L"Default__", 0) == 0) return false;
    // Walk the WHOLE Outer chain. Run 2 filtered only the immediate outer and removed 3
    // of 400, because a widget-tree TEMPLATE's immediate outer is a UWidgetTree just like
    // a live instance's is -- the discriminator is one level further up, where a template
    // reaches its WidgetBlueprintGeneratedClass and a live widget reaches a UUserWidget
    // instance and then the World/GameInstance.
    void* outer = R::OuterOf(o);
    for (int depth = 0; outer && depth < 8; ++depth) {
        if (R::ToString(R::NameOf(outer)).rfind(L"Default__", 0) == 0) return false;
        if (R::ClassNameOf(outer).find(L"BlueprintGeneratedClass") != std::wstring::npos)
            return false;
        outer = R::OuterOf(outer);
    }
    return true;
}

// Call a no-arg bool-returning UFunction. UE4 lays the return value out as the whole
// parameter frame for such a function, so a single bool is the correct frame.
bool CallBoolFn(void* obj, void* fn) {
    if (!obj || !fn) return false;
    bool ret = false;
    if (!R::CallFunction(obj, fn, &ret)) return false;
    return ret;
}

// mainPlayer.activeInterface, or null. Shared by the sample line and the round trip.
void* ActiveInterface() {
    void* mp = R::FindObjectByClass(L"mainPlayer_C");
    if (!mp) return nullptr;
    if (g_activeInterfaceOff == -2)
        g_activeInterfaceOff = R::FindPropertyOffset(R::ClassOf(mp), L"activeInterface");
    if (g_activeInterfaceOff < 0) return nullptr;
    void* w = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mp) + g_activeInterfaceOff);
    return (w && R::IsLive(w)) ? w : nullptr;
}

// GREEN half of M1's control pair. Reading "focused=0" forever is indistinguishable
// from a predicate that is hard-wired false, so once per run we take a live editable
// widget, give it Slate keyboard focus through the ENGINE'S OWN `SetKeyboardFocus`,
// and re-read. If the count does not become >=1 the predicate does not work and the
// whole arc's chosen invariant is dead.
//
// LIMIT, stated so it is not over-claimed: this proves the READ reflects Slate focus.
// It does NOT prove the game's own entry path (panel_SATconsole -> Enter Interface ->
// EditableTextBox.SetFocus) produces the same state -- that still wants a real
// in-game GREEN. It deliberately does NOT go through `Enter Interface`, which would
// also author bIsFocusable / SetInputMode* / bShowMouseCursor, i.e. the probe would be
// writing the surrounding state it is meant to observe independently.
bool g_focusTestDone = false;

void FocusRoundTrip() {
    if (g_focusTestDone || !g_fnSetKeyboardFocus || !g_fnHasKeyboardFocus) return;
    char v[8]{};
    if (!(::GetEnvironmentVariableA("VOTVCOOP_INPUT_PROBE_FOCUS", v, sizeof(v)) > 0 && v[0] == '1'))
        return;
    // Target a field of the UI THAT IS ACTUALLY ON SCREEN. Runs 2 and 3 took the first
    // live instance in object order and got `pcui_file` then `ui_resetSave` -- widgets
    // that exist but are not in the viewport, where SetKeyboardFocus is a no-op. Slate
    // focus is a property of the live widget tree, so the target must belong to the
    // currently-active interface.
    void* iface = ActiveInterface();
    if (!iface) return;  // no game UI open yet
    void* target = nullptr;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n && !target; ++i) {
        void* o = R::ObjectAt(i);
        if (!o) continue;
        void* cls = R::ClassOf(o);
        if (cls != g_clsEditableTextBox && cls != g_clsMultiLineText &&
            cls != g_clsMultiLineTextBox)
            continue;
        if (!R::IsLive(o) || !IsLiveInstance(o)) continue;
        for (void* up = R::OuterOf(o); up; up = R::OuterOf(up))
            if (up == iface) { target = o; break; }
    }
    if (!target) return;  // no live instance yet -- the owning game UI is not open
    static int sAttempts = 0;
    if (++sAttempts > 20) g_focusTestDone = true;  // stop retrying eventually
    // UWidget::HasKeyboardFocus tests the CACHED WRAPPER widget exactly. UMG wraps a
    // UEditableTextBox in an SObjectWidget around an SEditableTextBox, and Slate focus
    // lands on the inner one -- so the exact test can be false while the tree plainly
    // has focus. Read every focus accessor UWidget exposes, on the field AND on the
    // owning interface, so the run says WHICH ONE is the usable predicate rather than
    // just "no".
    const bool before = CallBoolFn(target, g_fnHasKeyboardFocus);
    R::CallFunction(target, g_fnSetKeyboardFocus, nullptr);
    const bool after = CallBoolFn(target, g_fnHasKeyboardFocus);
    const bool tDesc = CallBoolFn(target, g_fnHasFocusedDescendants);
    const bool tAny = CallBoolFn(target, g_fnHasAnyUserFocus);
    const bool iDesc = CallBoolFn(iface, g_fnHasFocusedDescendants);
    const bool iAny = CallBoolFn(iface, g_fnHasAnyUserFocus);
    const bool iKb = CallBoolFn(iface, g_fnHasKeyboardFocus);
    UE_LOGI("input_probe: FOCUS ACCESSORS after SetKeyboardFocus -- "
            "target{kb=%d desc=%d anyUser=%d} iface{kb=%d desc=%d anyUser=%d}",
            (int)after, (int)tDesc, (int)tAny, (int)iKb, (int)iDesc, (int)iAny);
    if (after || tDesc || tAny || iDesc || iAny || iKb) g_focusTestDone = true;
    UE_LOGI("input_probe: FOCUS ROUND-TRIP #%d on %s/%s -- before=%d after=%d  (%s)",
            sAttempts,
            Narrow(R::ToString(R::NameOf(R::OuterOf(target)))).c_str(),
            Narrow(R::ToString(R::NameOf(target))).c_str(), (int)before, (int)after,
            after ? "PREDICATE WORKS" : "PREDICATE DEAD -- HasKeyboardFocus never reports focus");
}

void Sample() {
    ResolveOnce();
    if (!g_fnHasKeyboardFocus) { UE_LOGW("input_probe: no HasKeyboardFocus UFunction"); return; }

    int total = 0, live = 0, focused = 0;
    std::wstring focusedName = L"-";
    std::wstring focusedOuter = L"-";
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o) continue;
        void* cls = R::ClassOf(o);
        if (cls != g_clsEditableTextBox && cls != g_clsMultiLineText &&
            cls != g_clsMultiLineTextBox)
            continue;
        if (!R::IsLive(o)) continue;
        ++total;
        if (!IsLiveInstance(o)) continue;
        ++live;
        if (CallBoolFn(o, g_fnHasKeyboardFocus)) {
            ++focused;
            if (focused == 1) {
                focusedName = R::ToString(R::NameOf(o));
                void* outer = R::OuterOf(o);
                if (outer) focusedOuter = R::ToString(R::NameOf(outer));
            }
        }
    }

    // M1b: the activeInterface secondary.
    void* activeIface = ActiveInterface();
    const bool ifaceHasFocusedDesc =
        activeIface ? CallBoolFn(activeIface, g_fnHasFocusedDescendants) : false;
    std::wstring ifaceName = L"-";
    if (activeIface) ifaceName = R::ClassNameOf(activeIface);

    // M5: control rotation yaw. A poll-based mouselook fed a frozen off-centre pointer
    // would drift monotonically; a stationary camera holds one value.
    float yaw = 0.f, pitch = 0.f;
    void* pc = R::FindObjectByClass(L"PlayerController");
    if (pc) {
        if (g_controlRotationOff == -2)
            g_controlRotationOff = R::FindPropertyOffset(R::ClassOf(pc), L"ControlRotation");
        if (g_controlRotationOff >= 0) {
            const float* rot = reinterpret_cast<const float*>(
                reinterpret_cast<uint8_t*>(pc) + g_controlRotationOff);
            pitch = rot[0];
            yaw = rot[1];
        }
    }

    UE_LOGI("input_probe: editable ofClass=%d instances=%d focused=%d focusedName=%s outer=%s | "
            "activeInterface=%p (%s) hasFocusedDesc=%d | ctrlRot pitch=%.2f yaw=%.2f",
            total, live, focused, Narrow(focusedName).c_str(), Narrow(focusedOuter).c_str(),
            activeIface, Narrow(ifaceName).c_str(),
            (int)ifaceHasFocusedDesc, pitch, yaw);

    FocusRoundTrip();
}

}  // namespace

bool IsArmed() { return Armed(); }

void NoteFrame() {
    if (!Armed()) return;
    static ULONGLONG sNext = 0;
    const ULONGLONG now = ::GetTickCount64();
    if (now < sNext) return;
    sNext = now + 1000;
    ue_wrap::game_thread::Post([] { Sample(); });
}

}  // namespace coop::dev::input_focus_probe
