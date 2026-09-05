// coop/input/input_owner.cpp -- who owns a typed key: the game (an open interface, or a focused
// menu field), the overlay, or a hotkey. See coop/input/input_owner.h.

#include "coop/input/input_owner.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#include "coop/player/players_registry.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"

namespace R = ue_wrap::reflection;

namespace coop::input::input_owner {
namespace {

// Published by TickGameThread, read from the WndProc's MayTakeKey. Two independent terms,
// decided separately and stale at different rates: the interface term is exact and
// re-evaluated live at a keystroke; the scan term is a 1 Hz conclusion about surfaces that
// cannot be evaluated inline. Fused into one bool, leaving an interface left it stale-true for
// up to a second, and the live re-evaluation then discarded a menu-field owner the scan had
// found.
std::atomic<bool> g_ifaceOwnsText{false};
std::atomic<bool> g_scanOwnsText{false};
std::atomic<bool> g_overlayOwnsText{false};
// The game thread has not been observed yet, so nothing is known and MayTakeKey says no.
std::atomic<bool> g_everTicked{false};

// Diagnostics only.
char g_ownerName[64] = "-";

// The widget the last full scan found holding focus, re-probed first on the next (focus is
// sticky). A CachedObjRef, never a bare pointer: it lives across scans, at the menu too.
ue_wrap::CachedObjRef g_lastOwner;

void* g_clsUserWidget = nullptr;
void* g_fnHasKeyboardFocus = nullptr;
// Resolved once; rejects widget templates (IsLiveWidgetInstance).
void* g_clsWidgetBPGC = nullptr;
void* g_fnHasUserFocusedDescendants = nullptr;
// The local PlayerController, resolved once per full scan (a UFunction call). Null means the
// descendant question cannot be asked, and then no ownership is claimed rather than falling
// back to an all-users test.
void* g_pcForScan = nullptr;
// Which accessor concluded the scan's yes; diagnostics.
const char* g_scanTerm = "-";
int32_t g_activeInterfaceOff = -2;
bool g_resolved = false;

void ResolveOnce() {
    if (g_resolved) return;
    g_resolved = true;
    g_clsUserWidget = R::FindClass(L"UserWidget");
    void* widgetCls = R::FindClass(L"Widget");
    if (widgetCls) {
        g_fnHasKeyboardFocus = R::FindFunction(widgetCls, L"HasKeyboardFocus");
        g_clsWidgetBPGC = R::FindClass(L"WidgetBlueprintGeneratedClass");
        g_fnHasUserFocusedDescendants =
            R::FindFunction(widgetCls, L"HasUserFocusedDescendants");
    }
    UE_LOGI("input_owner: resolve UserWidget=%p HasKeyboardFocus=%p "
            "HasUserFocusedDescendants=%p",
            g_clsUserWidget, g_fnHasKeyboardFocus, g_fnHasUserFocusedDescendants);
}

// The game-side answer, read from the game's own guard. mainPlayer's key handler forwards a
// typed key into the focused widget only while `activeInterface` is valid, and delivers it
// through WidgetInteraction's virtual user. So UWidget::HasKeyboardFocus, which asks about user
// 0, is blind to every in-world widget screen (the console, the laptop, the arcade, the TV, the
// radar) whose UMG lives in a widget component driven by that interaction component. The
// player inventory looked right under the old predicate only because setActiveInterface also
// focuses the same widget for user 0.

// The local pawn as last validated by the tick, never resolved from the WndProc. Registry::Local
// on a cold cache runs a rescan that calls GetController, a reflected UFunction; our
// ProcessEvent detour would then drain posted tasks at top level, running spawns, wire applies
// and the pump synchronously inside a WM_KEYDOWN callback, and the first keypress after a
// travel would pay for the refill. So the tick resolves and the keystroke path only
// re-validates. A CachedObjRef, probed per WndProc message, at the menu too.
ue_wrap::CachedObjRef g_localPawn;

// The pointer is derived once and handed back: the drill below can make the predicate true
// without resolving either the pawn or the offset, and a caller re-deriving them dereferenced
// null.
void* ActiveInterfaceFrom(void* mp) {
    if (!mp) return nullptr;
    if (g_activeInterfaceOff == -2) {
        g_activeInterfaceOff = R::FindPropertyOffset(R::ClassOf(mp), L"activeInterface");
        // A silent permanent negative latch is how a recook would regress this: the dominant term
        // dead for good and nothing saying so. Name-driven lookups are the version surface.
        if (g_activeInterfaceOff < 0)
            UE_LOGE("input_owner: `activeInterface` NOT FOUND on %ls -- the game-owns-text "
                    "term is permanently dead and every hotkey will steal typed keys "
                    "(GitHub issue #5). A renamed field after a game recook looks exactly "
                    "like this.", R::ClassNameOf(mp).c_str());
    }
    if (g_activeInterfaceOff < 0) return nullptr;
    void* iface = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mp) + g_activeInterfaceOff);
    // IsValid in the BP sense (non-null, not pending kill), the game's own guard; a stale pointer
    // to a torn-down interface would leave every hotkey dead until the next one opened.
    return (iface != nullptr && R::IsLive(iface)) ? iface : nullptr;
}

// The tick path: may resolve. Publishes the pawn for the keystroke path below.
void* ActiveInterfaceResolving() {
    void* mp = coop::players::Registry::Get().Local();
    g_localPawn.Set(mp);  // fresh from the registry (it validates) -- the Set contract
    return ActiveInterfaceFrom(mp);
}

// The keystroke path: memory reads only. A dead or unpublished pawn answers "no interface",
// failing toward the game like every other unknown here.
void* ActiveInterfaceCached() {
    void* mp = g_localPawn.Get();
    if (!mp) return nullptr;
    return ActiveInterfaceFrom(mp);
}

// The drill (VOTVCOOP_INPUT_OWNER_DRILL=1) forces the term true: the yes branch needs a person
// at a terminal, so without it the path would ship having only ever said no. Pass: the yes edge
// logs with activeInterface=1 and every text-consumable hotkey (T, V, tilde) goes dead; F-keys
// stay alive, since KeyCouldBeConsumedByText exempts them before this term is consulted, and
// that exemption is the tested behaviour.
bool DrillForcesInterface() {
    static int sDrill = -1;
    if (sDrill == -1) {
        char v[8]{};
        sDrill = (::GetEnvironmentVariableA("VOTVCOOP_INPUT_OWNER_DRILL", v, sizeof(v)) > 0 &&
                  v[0] == '1') ? 1 : 0;
    }
    return sDrill == 1;
}

// `resolving` picks the pawn path: the tick passes true; the keystroke path passes false and
// must never resolve.
bool InterfaceOwnsTextLive(bool resolving) {
    if (DrillForcesInterface()) return true;
    return (resolving ? ActiveInterfaceResolving() : ActiveInterfaceCached()) != nullptr;
}

bool CallBoolNoArgs(void* widget, void* fn) {
    if (!widget || !fn) return false;
    bool ret = false;
    if (!R::CallFunction(widget, fn, &ret)) return false;
    return ret;
}

// Whether a widget is a live instance rather than a template. Every
// WidgetBlueprintGeneratedClass stores a template tree, and templates dominate the UserWidget
// population; the Default__ test does not catch them, since a template's immediate Outer is a
// UWidgetTree like a live instance's. The discriminator is further up: a template's chain
// reaches its generated class, a live widget's a UUserWidget or the World. Without this the
// 1 Hz scan issued about 9,300 reflected HasKeyboardFocus dispatches per pass, in one frame.
// The cached class is pointer-compared; ClassNameOf allocates.
bool IsLiveWidgetInstance(void* o) {
    void* outer = R::OuterOf(o);
    for (int d = 0; outer && d < 8; ++d) {
        if (R::NameStartsWith(R::NameOf(outer), L"Default__")) return false;
        if (g_clsWidgetBPGC && R::ClassOf(outer) == g_clsWidgetBPGC) return false;
        outer = R::OuterOf(outer);
    }
    return true;
}
// User-0 Slate focus, on the widget itself or on anything inside it; both terms are
// user-0-scoped. HasKeyboardFocus is an exact-widget test, true only while focus sits on the
// user widget itself, which is what SetInputMode_GameAndUIEx leaves when a menu opens; the
// moment the player clicks into a field (a save-slot rename, the settings search) focus moves
// to a descendant and the exact test goes false, hence the second term. It must be the User
// variant: the all-users HasFocusedDescendants stays true for good on the resident atlas
// widget, focused by WidgetInteraction's virtual user, so after one visit to the console T, V
// and tilde were dead until restart. That virtual user must be invisible to this term and
// visible to the activeInterface term.
bool OwnsUserZeroFocus(void* widget) {
    if (CallBoolNoArgs(widget, g_fnHasKeyboardFocus)) { g_scanTerm = "kbfocus"; return true; }
    if (!g_fnHasUserFocusedDescendants || !g_pcForScan) return false;
    // The UFunction frame: { APlayerController* PlayerController; bool ReturnValue; }
    struct Frame { void* pc; bool ret; } f{g_pcForScan, false};
    if (!R::CallFunction(widget, g_fnHasUserFocusedDescendants, &f)) return false;
    if (f.ret) g_scanTerm = "user0-descendant";
    return f.ret;
}

// True when `cls` is UUserWidget or derives from it, by walking the chain: the text surfaces
// are BlueprintGeneratedClasses whose names are never enumerated here.
bool DerivesFromUserWidget(void* cls) {
    for (int depth = 0; cls && depth < 16; ++depth) {
        if (cls == g_clsUserWidget) return true;
        cls = R::SuperStructOf(cls);
    }
    return false;
}

void Remember(void* w) {
    // Called every 10 Hz tick while an interface is open, and ClassNameOf allocates, so a repeat is
    // skipped. Latched on the pointer and its class: slots are recycled, and a new widget at an old
    // address would otherwise keep the previous name.
    static void* sLast = nullptr;
    static void* sLastCls = nullptr;
    void* cls = R::ClassOf(w);
    if (w == sLast && cls == sLastCls) return;
    sLast = w;
    sLastCls = cls;
    const std::wstring n = R::ClassNameOf(w);
    size_t i = 0;
    for (; i + 1 < sizeof(g_ownerName) && i < n.size(); ++i)
        g_ownerName[i] = n[i] < 0x80 ? static_cast<char>(n[i]) : '?';
    g_ownerName[i] = '\0';
}

// The instrument, edge-only: it prints when the answer changes, with its inputs (who owns it,
// which term concluded it), so a wrong verdict is diagnosable from one line.
void LogOwnerEdge() {
    const bool iface = g_ifaceOwnsText.load(std::memory_order_relaxed);
    const bool scan  = g_scanOwnsText.load(std::memory_order_relaxed);
    static int sLast = -1;
    const int state = (iface ? 2 : 0) | (scan ? 1 : 0);
    if (sLast == state) return;
    sLast = state;
    UE_LOGI("input_owner: gameOwnsText -> %s (owner=%s, activeInterface=%d scan=%d via %s)",
            (iface || scan) ? "YES" : "no", g_ownerName, iface ? 1 : 0, scan ? 1 : 0,
            scan ? g_scanTerm : "-");
}

}  // namespace

// Two cadences. The fast path is a pointer read plus one UFunction call and covers every
// surface reached through Enter Interface (the console, the notebook, the laptop, the
// inventory). The full path walks GUObjectArray and catches the menu-only fields (the
// save-slot rename, the settings search); at 10 Hz that would be a per-frame full-array scan,
// so it runs at 1 Hz, and a field in one of those surfaces can be focused for up to a second
// before a hotkey stops taking its key.
void TickGameThread(bool doFullScan) {
    // The refresh floor for world_identity, and it comes first. CurrentWorld memoises on a 100 ms
    // timer and is refreshed by whoever calls it, which is not a floor; this tick is one: 10 Hz,
    // ungated, alive at the menu with no session. First, because this tick is posted from the
    // overlay's present path and presents stop for seconds during a world teardown; the first tick
    // after they resume must already know the new world, or it resolves the pawn against the
    // previous one, the dead-pawn read this term exists to refuse.
    (void)ue_wrap::world_identity::CurrentWorld();
    ResolveOnce();
    if (!g_fnHasKeyboardFocus || !g_clsUserWidget) {
        // Unresolved means unknown, and unknown reads as "the game might own text", so MayTakeKey
        // fails open.
        g_everTicked.store(false, std::memory_order_relaxed);
        return;
    }

    // The fast path: the game's own guard. Two field reads, no scan (the registry already holds the
    // validated local).
    void* iface = ActiveInterfaceResolving();
    // The world-identity instrument (VOTVCOOP_WORLD_ID_PROBE=1, a no-op otherwise) hangs here
    // because this is the mod's only ungated game-thread heartbeat, alive at the menu with no
    // session. The pawn it compares against is the one this file feeds the engine.
    ue_wrap::world_identity::TickProbe(g_localPawn.Raw());
    if (iface || DrillForcesInterface()) {
        if (iface) Remember(iface);
        else       std::snprintf(g_ownerName, sizeof(g_ownerName), "(drill)");
        g_ifaceOwnsText.store(true, std::memory_order_relaxed);
        g_everTicked.store(true, std::memory_order_relaxed);
        LogOwnerEdge();
        return;
    }
    // The interface term is exact, so its false is published; the scan term below is not re-run at
    // this cadence.
    g_ifaceOwnsText.store(false, std::memory_order_relaxed);

    if (!doFullScan) {
        // No interface owns focus; the last full scan's conclusion stands, since this cadence has
        // not looked at the surfaces the fast path cannot see.
        g_everTicked.store(true, std::memory_order_relaxed);
        return;
    }

    // The full path: any live UUserWidget holding keyboard focus, the surfaces with no
    // interface-driving owner.
    bool owns = false;
    g_scanTerm = "-";
    // The local PlayerController scopes the descendant test to user 0. One UFunction call per scan,
    // not per widget.
    {
        void* lp = coop::players::Registry::Get().Local();
        g_pcForScan = lp ? ue_wrap::engine::GetController(lp) : nullptr;
    }

    // The last owner first: focus is sticky, and a hit skips the sweep, which otherwise pays two
    // reflected calls per non-owning widget.
    void* lastOwner = g_lastOwner.Get();
    if (lastOwner && OwnsUserZeroFocus(lastOwner)) {
        owns = true;
        Remember(lastOwner);
    }

    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n && !owns; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || o == g_lastOwner.Raw()) continue;  // already asked (identity compare only)
        void* cls = R::ClassOf(o);
        if (!DerivesFromUserWidget(cls)) continue;
        // NameStartsWith: a ToString per widget per scan allocated.
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
        if (!R::IsLive(o)) continue;
        if (!IsLiveWidgetInstance(o)) continue;
        if (OwnsUserZeroFocus(o)) { owns = true; Remember(o); g_lastOwner.Set(o); }
    }
    if (!owns) g_lastOwner.Reset();
    if (!owns) g_ownerName[0] = '-', g_ownerName[1] = '\0';
    g_scanOwnsText.store(owns, std::memory_order_relaxed);
    g_everTicked.store(true, std::memory_order_relaxed);
    LogOwnerEdge();
}


void PublishOverlayOwnsText(bool owns) {
    g_overlayOwnsText.store(owns, std::memory_order_relaxed);
}

bool GameOwnsText() {
    return g_ifaceOwnsText.load(std::memory_order_relaxed) ||
           g_scanOwnsText.load(std::memory_order_relaxed);
}
bool OverlayOwnsText() { return g_overlayOwnsText.load(std::memory_order_relaxed); }

bool IsForeground() {
    HWND fg = ::GetForegroundWindow();
    if (!fg) return true;  // cannot tell -> do not break the hotkey
    DWORD pid = 0;
    ::GetWindowThreadProcessId(fg, &pid);
    return pid == ::GetCurrentProcessId();
}

// Could `vk` do something inside a focused text widget? Conservative in the game's favour:
// everything is text-consuming except the function keys and the bare modifiers. A text field
// uses letters, digits, punctuation, space, Enter, Backspace, Delete, Tab and the navigation
// keys, and the only family it provably ignores is F1 to F24; enumerating the consumed set
// would mis-answer the first layout not thought of, enumerating the ignored set fails toward
// the game.
bool KeyCouldBeConsumedByText(unsigned vk) {
    if (vk >= VK_F1 && vk <= VK_F24) return false;
    switch (vk) {
        case VK_SHIFT: case VK_CONTROL: case VK_MENU:
        case VK_LSHIFT: case VK_RSHIFT:
        case VK_LCONTROL: case VK_RCONTROL:
        case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
        case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
        case VK_SNAPSHOT: case VK_PAUSE:
            return false;
        default:
            return true;
    }
}

bool MayTakeKey(unsigned vk) {
    if (!IsForeground()) return false;
    // A key the game cannot turn into text is ours whoever owns the keyboard; F1 works inside the
    // console.
    if (!KeyCouldBeConsumedByText(vk)) return true;
    // Not yet observed from the game thread: unknown, so fail open toward the game.
    if (!g_everTicked.load(std::memory_order_relaxed)) return false;

    // Synchronous when possible: the WndProc detour runs on the game thread, and every caller is a
    // WndProc hotkey edge, so the dominant term is evaluated at the keystroke rather than read from
    // a republish up to a tick old. That closes both staleness windows: a character lost while
    // entering an interface, and every hotkey dead for up to a second after leaving one. Checked,
    // not assumed: off the game thread the published atomic is used.
    if (ue_wrap::game_thread::IsGameThread()) {
        // The interface term live; the scan term as published, since it walks GUObjectArray and
        // covers menu-only surfaces whose staleness costs a hotkey, never a character.
        if (InterfaceOwnsTextLive(/*resolving=*/false)) return false;
        return !g_scanOwnsText.load(std::memory_order_relaxed);
    }
    return !GameOwnsText();
}

const char* LastGameOwnerName() { return g_ownerName; }

}  // namespace coop::input::input_owner
