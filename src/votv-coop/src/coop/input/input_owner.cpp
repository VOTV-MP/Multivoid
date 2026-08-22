// coop/input/input_owner.cpp -- see coop/input/input_owner.h for WHY.

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

namespace R = ue_wrap::reflection;

namespace coop::input::input_owner {
namespace {

// Published by TickGameThread, read from the WndProc. (This said "the WndProc + poller
// threads" until 2026-07-31; MEASURED FALSE -- all five MayTakeKey() call sites are in
// WndProcDetour and nothing else reads GameOwnsText(). The pollers still use
// ui::input_focus::IsOverlayCapturingText. Moving them onto this arbiter is the RULE-2
// debt named below; until it lands, the atomic has exactly one consumer.) One independent
// bool each; relaxed is right (see the header's staleness note).
// TWO INDEPENDENT TERMS, not one fused marker with a discriminator beside it. They
// are published separately because they are DECIDED separately and go stale at
// different rates: the interface term is exact and can be re-evaluated live at a
// keystroke, while the scan term is a 1 Hz conclusion about surfaces that cannot be
// evaluated inline. Fusing them into one bool plus a "which term said so" flag was
// the first shape of this code and it had a real hole: leaving an interface left the
// fused bool stale-true with the flag stale-"interface", so for up to a second the
// live re-evaluation would clear it -- discarding a menu-field owner the scan had
// legitimately found. Same defect this header names one level up.
std::atomic<bool> g_ifaceOwnsText{false};
std::atomic<bool> g_scanOwnsText{false};
std::atomic<bool> g_overlayOwnsText{false};
// Never observed the game thread yet => we do not know => MayTakeKey must say no.
std::atomic<bool> g_everTicked{false};

// Diagnostics only.
char g_ownerName[64] = "-";

// The widget the last full scan found holding focus, re-probed first on the next one
// (focus is sticky). Diagnostics-adjacent but load-bearing for the scan's cost -- see
// the sweep. Cached across 1 Hz scans (incl. at the menu) -> CachedObjRef, never a
// bare-IsLive raw pointer (islive-zeroav design section 3).
ue_wrap::CachedObjRef g_lastOwner;

void* g_clsUserWidget = nullptr;
void* g_fnHasKeyboardFocus = nullptr;
void* g_fnHasUserFocusedDescendants = nullptr;
// The local PlayerController, resolved ONCE per full scan (it costs a UFunction call).
// Null means we cannot ask the descendant question at all -- and we then do not claim
// ownership, rather than falling back to an all-users test.
void* g_pcForScan = nullptr;
// Which accessor concluded the scan's YES. Diagnostics, but the kind that turns a
// repeat of this bug into one grep instead of one more hands-on round.
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
        g_fnHasUserFocusedDescendants =
            R::FindFunction(widgetCls, L"HasUserFocusedDescendants");
    }
    UE_LOGI("input_owner: resolve UserWidget=%p HasKeyboardFocus=%p "
            "HasUserFocusedDescendants=%p",
            g_clsUserWidget, g_fnHasKeyboardFocus, g_fnHasUserFocusedDescendants);
}

// ---- THE game-side answer, read from THE GAME'S OWN GUARD ----
//
// MEASURED 2026-07-31, mainPlayer's ubergraph (research/bp_reflection, decoded with
// kdec.py -- the block that handles every key):
//
//     CallFunc_IsValid_ReturnValue_99 = IsValid(activeInterface)
//     POP_FLOW_IF_NOT(CallFunc_IsValid_ReturnValue_99)      <-- the guard
//     WidgetInteraction.SetFocus(activeInterface)
//     WidgetInteraction.PressPointerKey[virt](key)
//     CallFunc_PressKey_ReturnValue = WidgetInteraction.PressKey[virt](key, false)
//
// The game forwards a typed key into the focused widget IFF `activeInterface` is valid,
// and it delivers it through a VIRTUAL USER (WidgetInteraction has its own
// VirtualUserIndex). That is why the obvious predicate cannot work here:
// `UWidget::HasKeyboardFocus()` asks about USER 0, so it is structurally blind to every
// in-world widget screen -- the SAT console, the laptop, the arcade, the TV, the radar,
// the portable PC -- whose UMG lives in a UWidgetComponent driven by mainPlayer's
// UWidgetInteractionComponent.
//
// The predicate used to be `iface && IsLive(iface) && HasKeyboardFocus(iface)`: it read
// the RIGHT field and then threw the answer away on a question the engine answers about
// a different user. GitHub issue #5's console is the case that got wrong; the player
// inventory is the case that made it look right, because `setActiveInterface` ALSO calls
// SetInputMode_GameAndUIEx, which focuses the same widget for user 0. Two delivery
// paths, one of them never modelled.

// The local pawn, as last validated BY THE TICK. Never resolved from the WndProc.
//
// `Registry::Local()` looks like a pure cache read and is not: on a cold cache it runs
// RescanLocal(), which calls E::GetController() -- a REFLECTED UFunction, i.e.
// ProcessEvent. Our own PE detour then sees a non-empty queue at top level and runs
// DrainPostedTasksAtTopLevel(), so spawns, wire applies and net_pump::Tick would execute
// synchronously INSIDE a WM_KEYDOWN callback, re-entering the engine at a point that is
// not a tick boundary. `t_inPump` does not protect this: a WndProc is not inside a pump.
// Reachable for real -- InvalidateLocal() on a level change or a respawn clears the
// cache, and the first keypress after that would pay for it. So the tick owns the
// resolve and the keystroke path only re-validates. CachedObjRef: probed per
// WndProc message INCLUDING at the menu -> the runner-up suspect of the
// IsLive/VEH finding (islive-zeroav design section 3).
ue_wrap::CachedObjRef g_localPawn;

// The pointer is derived ONCE and handed back, so no caller re-walks the pawn and the
// offset a second time -- doing that was a real null-deref, because the drill below can
// make the predicate true without ever resolving either, and a caller that re-derived
// them dereferenced `nullptr + (-2)`. Found by running the drill, which is what it is for.
void* ActiveInterfaceFrom(void* mp) {
    if (!mp) return nullptr;
    if (g_activeInterfaceOff == -2) {
        g_activeInterfaceOff = R::FindPropertyOffset(R::ClassOf(mp), L"activeInterface");
        // A permanent negative latch with no diagnostic is how a game recook silently
        // regresses issue #5: the dominant term goes dead forever and nothing says so.
        // Name-driven lookups are the version surface docs/VERSION_MIGRATION.md tracks.
        if (g_activeInterfaceOff < 0)
            UE_LOGE("input_owner: `activeInterface` NOT FOUND on %ls -- the game-owns-text "
                    "term is permanently dead and every hotkey will steal typed keys "
                    "(GitHub issue #5). A renamed field after a game recook looks exactly "
                    "like this.", R::ClassNameOf(mp).c_str());
    }
    if (g_activeInterfaceOff < 0) return nullptr;
    void* iface = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mp) + g_activeInterfaceOff);
    // IsValid() in the BP sense: a non-null, non-pending-kill object. The game's
    // guard is exactly this, so ours must be too -- a stale pointer to a torn-down
    // interface would leave every hotkey dead until the next one opens.
    return (iface != nullptr && R::IsLive(iface)) ? iface : nullptr;
}

// TICK path: may resolve. Publishes the pawn for the keystroke path below.
void* ActiveInterfaceResolving() {
    void* mp = coop::players::Registry::Get().Local();
    g_localPawn.Set(mp);  // fresh from the registry (it validates) -- the Set contract
    return ActiveInterfaceFrom(mp);
}

// KEYSTROKE path: pure memory reads (Alive() is array-slot reads only). A dead or
// not-yet-published pawn answers "no interface", which fails toward the game
// exactly like every other unknown here.
void* ActiveInterfaceCached() {
    void* mp = g_localPawn.Get();
    if (!mp) return nullptr;
    return ActiveInterfaceFrom(mp);
}

// DRILL. The YES branch of this predicate cannot be reached by the autonomous smoke
// -- entering an interface needs a human at a terminal -- so without this the whole
// "the game owns text" path would ship having only ever been observed saying no,
// which is indistinguishable from a path that is wired up wrong.
// VOTVCOOP_INPUT_OWNER_DRILL=1 forces the term true. PASS CRITERION: the YES edge
// appears in the log with activeInterface=1, and every TEXT-CONSUMABLE hotkey (T, V,
// tilde) goes dead. F-keys stay alive -- KeyCouldBeConsumedByText exempts them before
// this term is ever consulted, and that exemption IS the tested behaviour, not a leak
// in the drill. A criterion of "every hotkey dies" would be unmeetable by construction
// and would invite someone to delete the exemption to satisfy it.
bool DrillForcesInterface() {
    static int sDrill = -1;
    if (sDrill == -1) {
        char v[8]{};
        sDrill = (::GetEnvironmentVariableA("VOTVCOOP_INPUT_OWNER_DRILL", v, sizeof(v)) > 0 &&
                  v[0] == '1') ? 1 : 0;
    }
    return sDrill == 1;
}

// `resolving` picks which of the two pawn paths above is allowed. The tick passes
// true; the keystroke path passes false and must never resolve (see g_localPawn).
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

// USER-0 Slate focus, on the widget itself OR on anything inside it.
//
// BOTH terms are user-0-scoped, and that is the entire point. `HasKeyboardFocus` is an
// EXACT-widget test, so it answers true only while focus sits on the user widget itself
// -- which is what SetInputMode_GameAndUIEx leaves behind when a menu opens and nothing
// has been clicked. The moment the player clicks into a field (save-slot rename, the
// settings search) focus moves to a DESCENDANT and the exact test goes false, i.e. it
// fails in precisely the case where typing is happening. Hence the second term.
//
// IT MUST BE THE `User` VARIANT. This first shipped as `HasFocusedDescendants()`, which
// is ALL-USERS -- and the user REPORTED the consequence within the hour: after visiting
// the SAT console once, T / V / tilde were dead forever. Measured from the edge log, on
// both peers identically:
//
//   YES (owner=ui_console_C,       activeInterface=1 scan=0)   <- entering, correct
//   YES (owner=ui_consolesAtlas_C, activeInterface=0 scan=1)   <- after leaving, LATCHED
//
// The in-world screens live in a UWidgetComponent whose UMG is focused by
// WidgetInteraction's VIRTUAL user, and nothing ever clears that focus -- so an
// all-users descendant test on the permanently-resident atlas widget is true forever.
// That is the same virtual user the fix for issue #5 exists because of: it must be
// INVISIBLE to this term and VISIBLE to the activeInterface term. Asking about the
// local PlayerController's user is what separates them.
bool OwnsUserZeroFocus(void* widget) {
    if (CallBoolNoArgs(widget, g_fnHasKeyboardFocus)) { g_scanTerm = "kbfocus"; return true; }
    if (!g_fnHasUserFocusedDescendants || !g_pcForScan) return false;
    // UFunction frame: { APlayerController* PlayerController; bool ReturnValue; }
    struct Frame { void* pc; bool ret; } f{g_pcForScan, false};
    if (!R::CallFunction(widget, g_fnHasUserFocusedDescendants, &f)) return false;
    if (f.ret) g_scanTerm = "user0-descendant";
    return f.ret;
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
    // The fast path calls this every 10 Hz tick for as long as an interface is open,
    // not just on the edge -- and ClassNameOf allocates a std::wstring. Skip the repeat.
    // Latched on the pointer AND its class: UObject slots are recycled (that is why
    // IsLiveByIndex exists), so a new widget at an old address would otherwise keep the
    // previous name and make the one line that explains a run say the wrong thing.
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

// The instrument. Edge-only -- it prints when the ANSWER changes, so a wrong
// verdict is diagnosable from one line without spamming a 10 Hz log, and it
// carries its INPUTS (who owns it, and which of the two terms concluded it) not
// just its verdict. `LastGameOwnerName()` existed with ZERO consumers before
// 2026-07-31: the build already recorded who owned the keyboard and threw it away
// every tick, which is exactly why GATE 0 came back as an outcome nobody could
// interpret.
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

    // FAST PATH: the game's own guard. Two field reads, no UFunction call, no scan
    // (Registry::Local() is a validated cache -- the GUObjectArray walk that used to
    // sit here ran 10x/second for an answer the registry already held).
    void* iface = ActiveInterfaceResolving();
    if (iface || DrillForcesInterface()) {
        if (iface) Remember(iface);
        else       std::snprintf(g_ownerName, sizeof(g_ownerName), "(drill)");
        g_ifaceOwnsText.store(true, std::memory_order_relaxed);
        g_everTicked.store(true, std::memory_order_relaxed);
        LogOwnerEdge();
        return;
    }
    // The interface term is EXACT, so its false is a real false and is published as
    // one -- unlike the scan term below, which this cadence has not re-run.
    g_ifaceOwnsText.store(false, std::memory_order_relaxed);

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
    g_scanTerm = "-";
    // The local PlayerController scopes the descendant test to USER 0. One UFunction
    // call per scan (1 Hz), not per widget.
    {
        void* lp = coop::players::Registry::Get().Local();
        g_pcForScan = lp ? ue_wrap::engine::GetController(lp) : nullptr;
    }

    // Ask LAST FRAME'S OWNER FIRST. Focus is sticky -- the overwhelmingly common case
    // is that whoever owned it a second ago still does -- and a hit here skips the
    // whole sweep. Without this the sweep pays TWO reflected UFunction calls for every
    // non-owning widget, because `A || B` only short-circuits on the rare true.
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
        // NameStartsWith, not ToString(...).rfind: the latter built and destroyed a
        // std::wstring for every user widget on every scan. The zero-allocation
        // primitive is the one players_registry and reflection already use here.
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
        if (!R::IsLive(o)) continue;
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

// Could `vk` do something inside a focused text widget?
//
// The rule is deliberately CONSERVATIVE in the game's favour: everything is assumed
// text-consuming EXCEPT the function keys and the bare modifiers. That is not a
// hedge, it is the honest reading -- a text field does something with letters,
// digits, punctuation, space, Enter, Backspace, Delete, Tab and every arrow/Home/End,
// and the only large family it provably ignores is F1..F24. Enumerating the
// CONSUMED set instead would be a site list that silently mis-answers the first
// keyboard layout or widget we did not think of; enumerating the IGNORED set fails
// toward the game, which is the direction this whole file fails in.
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
    // A key the game cannot turn into text is ours regardless of who owns the
    // keyboard -- that is what keeps F1 working inside the console.
    if (!KeyCouldBeConsumedByText(vk)) return true;
    // Not yet observed from the game thread => UNKNOWN => fail open toward the game.
    if (!g_everTicked.load(std::memory_order_relaxed)) return false;

    // SYNCHRONOUS when we can be. `gate3` measured 2026-07-31 that WndProcDetour
    // runs ON THE GAME THREAD (overlay_diag::NoteWndProcThread, logged
    // isGameThread=1), and every caller of this function is a WndProc hotkey edge --
    // so the dominant term can be evaluated at the keystroke itself instead of read
    // from a republish that is up to a tick old. That closes BOTH staleness windows
    // the header used to document as unavoidable: ~100 ms of false->true (a player
    // who enters an interface and types inside the window loses the character) and
    // ~1 s of true->false (every hotkey dead for up to a second after leaving one,
    // because the fast cadence deliberately never stores false).
    //
    // The predicate is CHECKED, not assumed: off the game thread we fall back to the
    // published atomic rather than touching engine state from the wrong thread.
    if (ue_wrap::game_thread::IsGameThread()) {
        // The interface term LIVE (zero staleness); the scan term as published,
        // because it cannot be evaluated inline -- it walks GUObjectArray, and it
        // covers menu-only surfaces whose staleness costs a hotkey, never a
        // character.
        if (InterfaceOwnsTextLive(/*resolving=*/false)) return false;
        return !g_scanOwnsText.load(std::memory_order_relaxed);
    }
    return !GameOwnsText();
}

const char* LastGameOwnerName() { return g_ownerName; }

}  // namespace coop::input::input_owner
