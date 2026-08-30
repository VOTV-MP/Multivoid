// coop/dev/native_text_probe.cpp -- see coop/dev/native_text_probe.h for WHY.

#include "coop/dev/native_text_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <string>

namespace coop::dev::native_text_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace U  = ue_wrap::umg;
namespace FT = ue_wrap::ftext_utils;

// What we type. Two ASCII characters, deliberately not a word the field could
// plausibly contain for any other reason -- if these come back, they came from here.
constexpr wchar_t kTyped[] = L"MV";

enum class Step { Idle, Placed, Settle, Verdict, Done };
Step  g_step   = Step::Idle;
void* g_box    = nullptr;
void* g_panel  = nullptr;
int   g_waited = 0;

bool Armed() {
    static const bool s =
        coop::config::ResolveFlag(::coop::config_registry::rows::native_text_probe);
    return s;
}

// RESOLVE UP THE CHAIN, because `R::FindFunction` is EXACT-OWNER (reflection.h:200) and
// the two functions this probe most depends on are declared on `UWidget`, not on
// `UEditableTextBox` (UMG.hpp:1785 `SetKeyboardFocus`, :1802 `HasKeyboardFocus`).
//
// THIS BUG ALREADY PRODUCED A FALSE VERDICT ONCE, and it is recorded here rather than
// quietly fixed: the probe's first run reported `SetKeyboardFocus=0` and then declared
// the field could not take text. The field had never been focused -- the instrument
// failed to create the condition it was testing, so the NO it printed was its own. An
// instrument that cannot arm reports a negative indistinguishable from a real one.
void* FindFnClimbing(void* obj, const wchar_t* fn) {
    for (void* cls = R::ClassOf(obj); cls; cls = R::SuperStructOf(cls))
        if (void* f = R::FindFunction(cls, fn)) return f;
    return nullptr;
}

// Call a no-argument UFunction by name on `obj`. Returns false if it does not resolve,
// which is itself worth logging -- a missing SetKeyboardFocus is a different failure
// from a present one that does nothing.
bool CallNoArg(void* obj, const wchar_t* fn) {
    void* f = FindFnClimbing(obj, fn);
    if (!f) return false;
    ue_wrap::ParamFrame frame(f);
    return ue_wrap::Call(obj, frame);
}

bool CallBoolNoArg(void* obj, const wchar_t* fn, bool& out) {
    void* f = FindFnClimbing(obj, fn);
    if (!f) return false;
    ue_wrap::ParamFrame frame(f);
    if (!ue_wrap::Call(obj, frame)) return false;
    out = frame.Get<bool>(L"ReturnValue");
    return true;
}

// SetText / SetHintText both take one FText by value.
bool CallTextFn(void* obj, const wchar_t* fn, const wchar_t* param, const wchar_t* value) {
    void* f = FindFnClimbing(obj, fn);
    if (!f) return false;
    unsigned char text[FT::kFTextSize]{};
    if (!FT::MintFText(value, text)) return false;
    ue_wrap::ParamFrame frame(f);
    if (!frame.SetRaw(param, text, static_cast<int32_t>(sizeof(text)))) return false;
    return ue_wrap::Call(obj, frame);
}

// Read the field's own `Text` property back -- the ARTIFACT this probe exists to read.
std::wstring ReadFieldText(void* box) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(box), L"Text");
    if (off < 0) return L"<no Text property>";
    return FT::FTextToString(reinterpret_cast<const unsigned char*>(box) + off);
}

void Cleanup() {
    if (g_box && g_panel) U::RemoveChild(g_panel, g_box);
    g_box = nullptr;
    g_panel = nullptr;
}

}  // namespace

void Tick(void* panel) {
    if (!Armed() || g_step == Step::Done) return;

    switch (g_step) {
        case Step::Idle: {
            if (!panel) return;   // screen not built yet -- wait, do not fail
            void* cls = R::FindClass(L"EditableTextBox");
            if (!cls) {
                UE_LOGE("[native_text_probe] VERDICT: NO -- class `EditableTextBox` does "
                        "not resolve at all, so option A is dead before focus is even a "
                        "question. Every other rung below is unreachable.");
                g_step = Step::Done;
                return;
            }
            g_box = E::SpawnUObject(cls, panel);
            if (!g_box || !U::AddChild(panel, g_box)) {
                UE_LOGE("[native_text_probe] VERDICT: NO -- the box %s. A field that cannot "
                        "be built cannot be typed into.",
                        g_box ? "would not attach to the panel" : "would not spawn");
                Cleanup();
                g_step = Step::Done;
                return;
            }
            g_panel = panel;
            E::SetWidgetVisibility(g_box, 0);   // ESlateVisibility::Visible -- hit-testable

            const bool setHint = CallTextFn(g_box, L"SetHintText", L"InText",
                                            L"Enter an address [IP:Port]");
            const bool setText = CallTextFn(g_box, L"SetText", L"InText", L"");
            const bool focused = CallNoArg(g_box, L"SetKeyboardFocus");

            // Logged, but NOT the verdict: this is the exact predicate the 2026-07-31
            // finding says lies about a live field. It is here to tell a future reader
            // WHICH way it lied this time, not to decide anything.
            bool hasFocus = false;
            const bool focusReadable = CallBoolNoArg(g_box, L"HasKeyboardFocus", hasFocus);

            UE_LOGW("[native_text_probe] field built: SetHintText=%d SetText=%d "
                    "SetKeyboardFocus=%d | HasKeyboardFocus %s%d (advisory only)",
                    setHint ? 1 : 0, setText ? 1 : 0, focused ? 1 : 0,
                    focusReadable ? "" : "UNREADABLE ", hasFocus ? 1 : 0);

            // POST THE CHARACTERS OURSELVES. Waiting for a human to type would make a
            // silent run indistinguishable from a negative one.
            if (HWND hwnd = ::GetActiveWindow()) {
                for (const wchar_t* p = kTyped; *p; ++p)
                    ::PostMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(*p), 1);
                UE_LOGW("[native_text_probe] posted WM_CHAR '%ls' to the game window",
                        kTyped);
            } else {
                UE_LOGE("[native_text_probe] VERDICT: INCONCLUSIVE -- no active window to "
                        "post to, so nothing was typed and a negative below would be the "
                        "harness's, not the widget's.");
                Cleanup();
                g_step = Step::Done;
                return;
            }
            g_step = Step::Placed;
            return;
        }
        case Step::Placed:
            // One tick for the message to be pumped and Slate to route it.
            g_step  = Step::Settle;
            g_waited = 0;
            return;
        case Step::Settle:
            if (++g_waited < 8) return;   // the same eight-tick budget the browser selftest trusts
            g_step = Step::Verdict;
            return;
        case Step::Verdict: {
            const std::wstring got = ReadFieldText(g_box);
            const bool pass = (got.find(kTyped) != std::wstring::npos);
            if (pass)
                UE_LOGW("[native_text_probe] VERDICT: YES -- a UEditableTextBox in a "
                        "hand-wired tree TOOK typed text. Field reads '%ls' after posting "
                        "'%ls'. Native text entry is buildable; the browser's address "
                        "field and the Host window's world NAME both unblock on this.",
                        got.c_str(), kTyped);
            else
                UE_LOGE("[native_text_probe] VERDICT: NO -- the field was built, made "
                        "Visible and focused, %zu character(s) were posted, and its Text "
                        "still reads '%ls'. Slate does not route keystrokes into a "
                        "never-Initialize()d tree, so a native field is not the mechanism "
                        "and the address must come from our own WM_CHAR capture or from a "
                        "surface that already has input.",
                        sizeof(kTyped) / sizeof(kTyped[0]) - 1, got.c_str());
            Cleanup();
            g_step = Step::Done;
            return;
        }
        case Step::Done:
            return;
    }
}

}  // namespace coop::dev::native_text_probe
