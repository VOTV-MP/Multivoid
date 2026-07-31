// coop/input/input_owner.cpp -- see coop/input/input_owner.h for WHY.

#include "coop/input/input_owner.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

namespace R = ue_wrap::reflection;

namespace coop::input::input_owner {
namespace {

// Published by TickGameThread, read from the WndProc. (This said "the WndProc + poller
// threads" until 2026-07-31; MEASURED FALSE -- all five MayTakeKey() call sites are in
// WndProcDetour and nothing else reads GameOwnsText(). The pollers still use
// ui::input_focus::IsOverlayCapturingText. Moving them onto this arbiter is the RULE-2
// debt named below; until it lands, the atomic has exactly one consumer.) One independent
// bool each; relaxed is right (see the header's staleness note).
std::atomic<bool> g_gameOwnsText{false};
std::atomic<bool> g_overlayOwnsText{false};
// Never observed the game thread yet => we do not know => MayTakeKey must say no.
std::atomic<bool> g_everTicked{false};

// Diagnostics only.
char g_ownerName[64] = "-";

void* g_clsUserWidget = nullptr;
void* g_fnHasKeyboardFocus = nullptr;
int32_t g_activeInterfaceOff = -2;
bool g_resolved = false;

void ResolveOnce() {
    if (g_resolved) return;
    g_resolved = true;
    g_clsUserWidget = R::FindClass(L"UserWidget");
    void* widgetCls = R::FindClass(L"Widget");
    if (widgetCls) g_fnHasKeyboardFocus = R::FindFunction(widgetCls, L"HasKeyboardFocus");
    UE_LOGI("input_owner: resolve UserWidget=%p Widget::HasKeyboardFocus=%p",
            g_clsUserWidget, g_fnHasKeyboardFocus);
}

bool HasKeyboardFocus(void* widget) {
    if (!widget || !g_fnHasKeyboardFocus) return false;
    bool ret = false;
    if (!R::CallFunction(widget, g_fnHasKeyboardFocus, &ret)) return false;
    return ret;
}

// True when `cls` is UUserWidget or derives from it. The class chain is walked rather
// than name-matched: the 26 text surfaces are all `*_C` BlueprintGeneratedClasses whose
// names we must never enumerate (that is the site list this design exists to avoid).
bool DerivesFromUserWidget(void* cls) {
    for (int depth = 0; cls && depth < 16; ++depth) {
        if (cls == g_clsUserWidget) return true;
        cls = R::SuperStructOf(cls);
    }
    return false;
}

void Remember(void* w) {
    const std::wstring n = R::ClassNameOf(w);
    size_t i = 0;
    for (; i + 1 < sizeof(g_ownerName) && i < n.size(); ++i)
        g_ownerName[i] = n[i] < 0x80 ? static_cast<char>(n[i]) : '?';
    g_ownerName[i] = '\0';
}

}  // namespace

// Two cadences, because the two paths cost wildly different amounts and cover different
// things. The FAST path is a pointer read plus ONE UFunction call and covers every surface
// reachable through `Enter Interface` -- the console, the notebook, the laptop, the
// inventory: i.e. everything the reporter and the user actually named. The FULL path walks
// GUObjectArray and is what catches the 8 census outliers (save-slot rename, settings
// search, ...); at 10 Hz that would be a per-frame-class full-array scan, which this
// project bans on measurement, so it runs at 1 Hz. Consequence, stated rather than hidden:
// a field in one of those 8 surfaces can be focused for up to ~1 s before we notice, and
// during that window a hotkey could still take its key. Everything else is <=100 ms.
void TickGameThread(bool doFullScan) {
    ResolveOnce();
    if (!g_fnHasKeyboardFocus || !g_clsUserWidget) {
        // Unresolved means UNKNOWN, and unknown must read as "the game might own text"
        // so MayTakeKey fails open. Deliberately NOT a silent false.
        g_everTicked.store(false, std::memory_order_relaxed);
        return;
    }

    // FAST PATH: mainPlayer.activeInterface. One pointer read; when a game interface is
    // open this answers in a single UFunction call and the scan below never runs.
    void* mp = R::FindObjectByClass(L"mainPlayer_C");
    if (mp) {
        if (g_activeInterfaceOff == -2)
            g_activeInterfaceOff = R::FindPropertyOffset(R::ClassOf(mp), L"activeInterface");
        if (g_activeInterfaceOff >= 0) {
            void* iface = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mp) +
                                                    g_activeInterfaceOff);
            if (iface && R::IsLive(iface) && HasKeyboardFocus(iface)) {
                Remember(iface);
                g_gameOwnsText.store(true, std::memory_order_relaxed);
                g_everTicked.store(true, std::memory_order_relaxed);
                return;
            }
        }
    }

    if (!doFullScan) {
        // The fast path said "no interface owns focus". Keep whatever the last full scan
        // concluded rather than asserting false: this cadence has not looked at the 8
        // surfaces the fast path cannot see, and claiming they are clear would be a
        // marker true about the FAST PATH and false about the input path.
        g_everTicked.store(true, std::memory_order_relaxed);
        return;
    }

    // FULL PATH: any live UUserWidget holding keyboard focus. This is the invariant --
    // the census found 8 of the 26 text surfaces with no interface-driving owner, so the
    // fast path alone would miss save-slot renaming and the settings search.
    bool owns = false;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n && !owns; ++i) {
        void* o = R::ObjectAt(i);
        if (!o) continue;
        void* cls = R::ClassOf(o);
        if (!DerivesFromUserWidget(cls)) continue;
        if (R::ToString(R::NameOf(o)).rfind(L"Default__", 0) == 0) continue;
        if (!R::IsLive(o)) continue;
        if (HasKeyboardFocus(o)) { owns = true; Remember(o); }
    }
    if (!owns) g_ownerName[0] = '-', g_ownerName[1] = '\0';
    g_gameOwnsText.store(owns, std::memory_order_relaxed);
    g_everTicked.store(true, std::memory_order_relaxed);
}

void PublishOverlayOwnsText(bool owns) {
    g_overlayOwnsText.store(owns, std::memory_order_relaxed);
}

bool GameOwnsText() { return g_gameOwnsText.load(std::memory_order_relaxed); }
bool OverlayOwnsText() { return g_overlayOwnsText.load(std::memory_order_relaxed); }

bool IsForeground() {
    HWND fg = ::GetForegroundWindow();
    if (!fg) return true;  // cannot tell -> do not break the hotkey
    DWORD pid = 0;
    ::GetWindowThreadProcessId(fg, &pid);
    return pid == ::GetCurrentProcessId();
}

bool MayTakeKey() {
    if (!IsForeground()) return false;
    // Not yet observed from the game thread => UNKNOWN => fail open toward the game.
    if (!g_everTicked.load(std::memory_order_relaxed)) return false;
    return !g_gameOwnsText.load(std::memory_order_relaxed);
}

const char* LastGameOwnerName() { return g_ownerName; }

}  // namespace coop::input::input_owner
