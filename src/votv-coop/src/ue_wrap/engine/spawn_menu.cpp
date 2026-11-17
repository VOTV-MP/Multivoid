// ue_wrap/engine/spawn_menu.cpp -- see the header.

#include "ue_wrap/engine/spawn_menu.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/engine/engine.h"      // GetController -- the local player's PlayerController for input mode
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_singleton.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::spawn_menu {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;
namespace sg = ue_wrap::script_gate;

namespace {

// The game closes this menu with a native SetVisibility(Collapsed) reached only from the Q input event's
// release, so that event's body ending is the engine's own signal that the menu may be gone.
UE_FACT_FN(kRowQRelease, "spawn_menu", L"mainPlayer_C", L"InpActEvt_spawnmenu_K2Node_InputActionEvent_3", Declared);
constexpr int kTagQRelease = 0x534D4E55;  // 'SMNU'

// The widget's visibility byte, which Open() drives: the offset is the class's layout, the same in
// every world.
int32_t g_visOff = -2;  // UWidget.Visibility byte offset (-2 unresolved, -1 none)

// The live (non-CDO) ui_spawnmenu_C widget instance. propProcessor creates ONE per world at
// startup (`ExecuteUbergraph_propProcessor: WidgetBlueprintLibrary.Create(ui_spawnmenu_C) +
// AddToViewport`) in EVERY gamemode -- verified live: in story it exists at Visibility=1
// (Collapsed). nullptr only if the world hasn't created it yet.
void* FindSpawnMenuWidget() {
    return world_singleton::Find(L"ui_spawnmenu_C");
}

// ESlateVisibility byte of `widget` (0=Visible, 1=Collapsed, 2=Hidden, ...), or -3 if the
// offset is unresolved. Used as the before/after open diagnostic.
int ReadVis(void* widget) {
    if (!widget) return -3;
    if (g_visOff == -2) g_visOff = R::FindPropertyOffset(R::ClassOf(widget), L"Visibility");
    if (g_visOff < 0) return -3;
    return *reinterpret_cast<const uint8_t*>(reinterpret_cast<const uint8_t*>(widget) + g_visOff);
}

// A visibility byte that draws: Visible, HitTestInvisible or SelfHitTestInvisible.
bool Shown(int vis) { return vis == 0 || vis == 3 || vis == 4; }

// Open() ran and neither Close() nor the game's own close has since. Written on the game thread, read by the key
// watcher.
std::atomic<bool> g_open{false};

// Whether the menu is up, and with it the script gate: the watch on the game's close needs the gate while the menu
// is up and nothing else does, so a solo game pays for it only then (a coop session holds the gate anyway). Game
// thread.
void SetOpen(bool open) {
    static bool sHeld = false;
    if (open && !sHeld) {
        sHeld = true;
        sg::Acquire("the spawn menu");
    } else if (!open && sHeld) {
        sHeld = false;
        sg::Release("the spawn menu");
    }
    g_open.store(open, std::memory_order_release);
}

// After the game's Q release ran: when the widget is no longer drawn the game closed it, so the open this layer
// owes a close for is over and the key watcher can end. Read once, at the event.
void OnQRelease(const sg::Call&) {
    if (!g_open.load(std::memory_order_acquire)) return;
    if (Shown(ReadVis(FindSpawnMenuWidget()))) return;
    SetOpen(false);
    UE_LOGI("spawn_menu: the game closed the menu itself -- no close is owed");
}

// The widget belongs to the world that was current: a world change destroys it with no Q release to say so, so the open
// is forgotten here, and with it the gate hold.
void OnWorldChange() noexcept {
    if (!g_open.load(std::memory_order_acquire)) return;
    SetOpen(false);
    UE_LOGI("spawn_menu: the world changed -- the menu is gone with it, no close is owed");
}

// Watch the game's close and the world's change, registered once. Game thread.
void ArmCloseWatch() {
    static bool sArmed = false;
    if (sArmed) return;
    sArmed = true;
    if (!ue_wrap::world_identity::AddWorldChangeListener(&OnWorldChange))
        UE_LOGE("spawn_menu: the world-change list is full -- a menu left open across a world change stays owed a close");
    if (!sg::WatchName(kRowQRelease, kTagQRelease, nullptr, &OnQRelease))
        UE_LOGE("spawn_menu: the watch on %ls::%ls was refused -- the menu stays owed a close until Q is pressed",
                kRowQRelease.Class(), kRowQRelease.Name());
    sg::ResolvePendingNames();
}

// The menu's search box holds the keyboard: a Q typed there is a letter of the search.
bool SearchHasKeyboard(void* widget) {
    static int32_t sBoxOff = -2;
    if (sBoxOff < 0) sBoxOff = R::FindPropertyOffset(R::ClassOf(widget), L"textbox_search");   // a miss is asked again
    if (sBoxOff < 0) return false;
    void* box = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(widget) + sBoxOff);
    if (!box) return false;
    static void* sHasFocusFn = nullptr;
    if (!sHasFocusFn) {
        void* widgetCls = R::FindClass(L"Widget");
        sHasFocusFn = widgetCls ? R::FindFunction(widgetCls, L"HasKeyboardFocus") : nullptr;
    }
    if (!sHasFocusFn) return false;
    ue_wrap::ParamFrame f(sHasFocusFn);
    if (!f.valid() || !ue_wrap::Call(box, f)) return false;
    return f.Get<bool>(L"ReturnValue");
}

}  // namespace

bool Open(void* localPlayer) {
    if (!localPlayer) {
        UE_LOGW("spawn_menu::Open: no local mainPlayer_C (not in a world yet) -- not opened");
        return false;
    }

    // The vanilla open guard: the game's own open block no-ops when `activeInterface` is valid,
    // another UI being up. Honour it so we never stack the spawn menu over another menu. The
    // reflection offset is cached; a miss just skips the guard.
    {
        static int32_t sActiveIfaceOff = -2;
        if (sActiveIfaceOff == -2) sActiveIfaceOff = R::FindPropertyOffset(R::ClassOf(localPlayer), L"activeInterface");
        if (sActiveIfaceOff >= 0) {
            void* iface = *reinterpret_cast<void* const*>(
                reinterpret_cast<const uint8_t*>(localPlayer) + sActiveIfaceOff);
            if (iface) {
                UE_LOGW("spawn_menu::Open: mainPlayer.activeInterface is SET (%p) -- another UI is "
                        "open; not opening the spawn menu over it (close it first)", iface);
                return false;
            }
        }
    }

    // Two routes into the game's own open path are dead ends, so this opens the menu directly.
    // Calling the input-event UFunction (InpActEvt_spawnmenu_..._2) is a no-op: the engine's input
    // system wires the forward to the open ubergraph, not to the stub body, so the dispatch left
    // the widget collapsed. And driving ExecuteUbergraph_mainPlayer at the open entry runs the
    // game's own guards, one of which is lib_C::isBuoyant -- a capability check that is false in
    // story mode, and whose flying branch can quit the game (see the header). What follows
    // therefore reproduces the open block's effects on the widget the game already created:
    // visibility, the widget's own opened(), then the input mode and cursor. No asset edit
    // (RULE 3).
    void* widget = FindSpawnMenuWidget();
    if (!widget) {
        UE_LOGW("spawn_menu::Open: no live ui_spawnmenu_C widget in this world yet -- cannot open");
        return false;
    }
    const int visBefore = ReadVis(widget);  // resolves g_visOff
    // Call the NATIVE UWidget::SetVisibility (resolved on the native "Widget" UClass -- FindFunction
    // on the BP class doesn't reach inherited native UFunctions) so the change propagates to the live
    // Slate widget (a raw byte write to Visibility leaves the cached SWidget collapsed -- proven: the
    // byte went 1->0 but the menu still didn't render). Then run the widget's own opened() content
    // setup.
    static void* sSetVisFn = nullptr;
    if (!sSetVisFn) {
        void* widgetCls = R::FindClass(L"Widget");  // UWidget
        sSetVisFn = widgetCls ? R::FindFunction(widgetCls, L"SetVisibility") : nullptr;
    }
    if (sSetVisFn) {
        ue_wrap::ParamFrame f(sSetVisFn);
        if (f.valid()) {
            // SelfHitTestInvisible, the value the game's own open block sets: the menu's buttons take
            // clicks and its full-screen root passes the rest to the world.
            f.Set<uint8_t>(L"InVisibility", 4);
            ue_wrap::Call(widget, f);
        }
    } else if (g_visOff >= 0) {
        *(reinterpret_cast<uint8_t*>(widget) + g_visOff) = 4;  // fallback: raw byte (may not render)
        UE_LOGW("spawn_menu::Open: native UWidget::SetVisibility unresolved -- raw byte fallback (Slate may not render)");
    }
    // opened() on the widget's own class, through the memoised lookup: it holds the answer by slot
    // and serial, so a class a world change replaced is asked again.
    if (void* openedFn = R::FindDispatchFunctionCached(R::ClassOf(widget), L"opened")) {
        ue_wrap::ParamFrame f(openedFn);
        if (f.valid()) ue_wrap::Call(widget, f);
    }

    // Cursor + UI input so the rendered menu is CLICKABLE (the native open path's
    // SetInputMode_GameAndUIEx(PC, spawnmenu, ...) @12494 + show the mouse cursor). Without this the
    // menu renders but the game stays GameOnly -> no cursor, can't click a prop to spawn it.
    if (void* pc = ue_wrap::engine::GetController(localPlayer)) {
        static void* sWblCdo = nullptr;
        static void* sSetInputFn = nullptr;
        if (!sWblCdo) sWblCdo = R::FindClassDefaultObject(L"WidgetBlueprintLibrary");
        if (sWblCdo && !sSetInputFn) sSetInputFn = R::FindFunction(R::ClassOf(sWblCdo), L"SetInputMode_GameAndUIEx");
        if (sWblCdo && sSetInputFn) {
            ue_wrap::ParamFrame f(sSetInputFn);
            if (f.valid()) {
                f.Set<void*>(L"PlayerController", pc);
                f.Set<void*>(L"InWidgetToFocus", widget);
                f.Set<uint8_t>(L"InMouseLockMode", 0);            // EMouseLockMode::DoNotLock
                f.Set<uint8_t>(L"bHideCursorDuringCapture", 0);   // keep the cursor
                ue_wrap::Call(sWblCdo, f);
            }
        }
        static int32_t sCursorOff = -2;
        if (sCursorOff == -2) sCursorOff = R::FindPropertyOffset(R::ClassOf(pc), L"bShowMouseCursor");
        if (sCursorOff >= 0) *(reinterpret_cast<uint8_t*>(pc) + sCursorOff) = 1;
    }

    const int visAfter = ReadVis(widget);
    ArmCloseWatch();
    SetOpen(true);
    UE_LOGI("spawn_menu::Open: native SetVisibility + opened() + GameAndUI input -- "
            "visibility %d -> %d (4=SelfHitTestInvisible)", visBefore, visAfter);
    return true;
}

bool IsOpen() { return g_open.load(std::memory_order_acquire); }

bool Close(void* localPlayer) {
    SetOpen(false);
    // (1) THE LOAD-BEARING UN-STICK: restore SetInputMode_GameOnly + hide the cursor. Open() leaves
    // the player in GameAndUI/cursor mode (spawn_menu.cpp Open() @ SetInputMode_GameAndUIEx + cursor);
    // without this the world interaction/use trace stays dead. Do it FIRST and unconditionally (even
    // if the widget lookup below fails), so input can never stay trapped. Mirrors vanilla's close
    // block (ExecuteUbergraph_mainPlayer @11555). SetInputMode_GameOnly(APlayerController*) -- single
    // param 'PlayerController' (UMG.hpp).
    if (localPlayer) {
        if (void* pc = ue_wrap::engine::GetController(localPlayer)) {
            static void* sWblCdo     = nullptr;
            static void* sGameOnlyFn = nullptr;
            if (!sWblCdo) sWblCdo = R::FindClassDefaultObject(L"WidgetBlueprintLibrary");
            if (sWblCdo && !sGameOnlyFn)
                sGameOnlyFn = R::FindFunction(R::ClassOf(sWblCdo), L"SetInputMode_GameOnly");
            if (sWblCdo && sGameOnlyFn) {
                ue_wrap::ParamFrame f(sGameOnlyFn);
                if (f.valid()) {
                    f.Set<void*>(L"PlayerController", pc);
                    ue_wrap::Call(sWblCdo, f);
                }
            }
            static int32_t sCursorOff = -2;
            if (sCursorOff == -2) sCursorOff = R::FindPropertyOffset(R::ClassOf(pc), L"bShowMouseCursor");
            if (sCursorOff >= 0) *(reinterpret_cast<uint8_t*>(pc) + sCursorOff) = 0;
        }
    }

    // (2) Collapse the widget (native UWidget::SetVisibility, the inverse of Open()'s native show).
    void* widget = FindSpawnMenuWidget();
    if (widget) {
        static void* sSetVisFn = nullptr;
        if (!sSetVisFn) {
            void* widgetCls = R::FindClass(L"Widget");  // UWidget
            sSetVisFn = widgetCls ? R::FindFunction(widgetCls, L"SetVisibility") : nullptr;
        }
        if (sSetVisFn) {
            ue_wrap::ParamFrame f(sSetVisFn);
            if (f.valid()) {
                f.Set<uint8_t>(L"InVisibility", 1);  // ESlateVisibility::Collapsed
                ue_wrap::Call(widget, f);
            }
        } else if (g_visOff >= 0) {
            *(reinterpret_cast<uint8_t*>(widget) + g_visOff) = 1;  // fallback: raw byte
        }
    }

    UE_LOGI("spawn_menu::Close: restored GameOnly input + hid cursor%s",
            widget ? " + collapsed widget" : " (no widget)");
    return true;
}

bool Toggle(void* localPlayer) {
    // Decide from GROUND TRUTH (the live Visibility byte), never a cached open-flag -- so a Q press
    // does the right thing even if the game closed the menu via its own path.
    void* widget = FindSpawnMenuWidget();
    if (widget && Shown(ReadVis(widget))) return CloseUnlessTyping(localPlayer);  // drawn -> dismiss
    return Open(localPlayer);                                       // collapsed / not-yet-created -> open
}

bool CloseUnlessTyping(void* localPlayer) {
    void* widget = FindSpawnMenuWidget();
    if (widget && Shown(ReadVis(widget)) && SearchHasKeyboard(widget)) return false;
    return Close(localPlayer);
}

}  // namespace ue_wrap::spawn_menu
